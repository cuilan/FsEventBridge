#include "fseventbridge.h"
#include <sys/fanotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <poll.h>
#include <liburing.h>

static feb_io_uring_t g_io_uring = { .ring_fd = -1 };

int io_uring_init(feb_io_uring_t *ctx) {
    if (ctx->ring_fd >= 0) return 0;
    
    int ret = io_uring_queue_init(32, &ctx->ring, 0);
    if (ret < 0) {
        LOG_ERROR("io_uring_queue_init failed: %s", strerror(-ret));
        return -1;
    }
    ctx->ring_fd = 1;
    return 0;
}

void io_uring_cleanup(feb_io_uring_t *ctx) {
    if (ctx->ring_fd >= 0) {
        io_uring_queue_exit(&ctx->ring);
        ctx->ring_fd = -1;
    }
}

// 这些事件位必须配合 fanotify_init(FAN_REPORT_FID) 才能在内核接受 mark
// 当前实现暂未启用 FID 模式，因此遇到这些位时主动剔除并告警，避免 EINVAL
#define FEB_FID_REQUIRED_MASK \
    (FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO)

static uint32_t build_event_mask(feb_config_t *config) {
    uint32_t mask = (config->event_mask != 0) ? config->event_mask : FAN_CLOSE_WRITE;

    uint32_t fid_bits = mask & FEB_FID_REQUIRED_MASK;
    if (fid_bits) {
        LOG_WARN("Stripping events that require FAN_REPORT_FID "
                 "(CREATE/DELETE/MOVED_FROM/MOVED_TO): mask 0x%x. "
                 "These events will be supported once FID mode is enabled.",
                 fid_bits);
        mask &= ~FEB_FID_REQUIRED_MASK;
    }

    if (mask == 0) {
        LOG_WARN("No usable events in mask, falling back to FAN_CLOSE_WRITE");
        mask = FAN_CLOSE_WRITE;
    }
    return mask;
}

// 初始化 fanotify 句柄
int monitor_init(const feb_config_t *config) {
    // 1. 初始化 fanotify 组
    // FAN_CLASS_NOTIF: 普通通知模式，不阻塞写操作
    // O_RDONLY | O_CLOEXEC: 句柄只读，且在 exec 时自动关闭
    int fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY | O_CLOEXEC);
    if (fan_fd == -1) {
        LOG_ERROR("fanotify_init failed");
        return -1;
    }

    // 2. 设置监控标记 (递归监控的核心)
    // FAN_MARK_ADD: 添加新标记
    // FAN_MARK_FILESYSTEM: 监控整个文件系统 (实现递归的关键)
    uint64_t mask = build_event_mask((feb_config_t *)config);
    unsigned int flags = FAN_MARK_ADD | FAN_MARK_FILESYSTEM;

    if (fanotify_mark(fan_fd, flags, mask, AT_FDCWD, config->monitor_path) == -1) {
        LOG_ERROR("fanotify_mark failed");
        close(fan_fd);
        return -1;
    }

    // 3. 如果启用 io_uring，初始化
    if (config->use_io_uring) {
        if (io_uring_init(&g_io_uring) == 0) {
            LOG_INFO("io_uring initialized successfully");
        } else {
            LOG_WARN("io_uring initialization failed, falling back to sync I/O");
        }
    }

    return fan_fd;
}

// 从文件描述符获取真实路径 (利用 /proc/self/fd)
static int get_path_from_fd(int fd, char *path, size_t size) {
    char procp[64];
    snprintf(procp, sizeof(procp), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(procp, path, size - 1);
    if (len != -1) {
        path[len] = '\0';
        return 0;
    }
    path[0] = '\0';
    return -1;
}

// 判断 path 是否处于 prefix 指向的目录子树下
// 支持 prefix 末尾带不带 '/' 两种写法；空 prefix 视为不匹配
static bool path_under_prefix(const char *path, const char *prefix) {
    if (prefix == NULL || prefix[0] == '\0') return false;

    size_t plen = strlen(prefix);
    // 去除 prefix 末尾多余的 '/'，便于统一处理
    while (plen > 1 && prefix[plen - 1] == '/') plen--;

    if (strncmp(path, prefix, plen) != 0) return false;

    // 命中边界要求：path 等于 prefix，或紧随其后是 '/'
    char next = path[plen];
    return next == '\0' || next == '/';
}

// 综合过滤：扩展名 + 路径前缀
static bool should_skip(const char *path, const feb_config_t *config) {
    // 路径前缀过滤
    for (int i = 0; i < config->exclude_paths_count; i++) {
        if (path_under_prefix(path, config->exclude_paths[i])) return true;
    }

    // 扩展名过滤
    if (config->exclude_exts_count > 0) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            for (int i = 0; i < config->exclude_exts_count; i++) {
                if (strcmp(dot, config->exclude_exts[i]) == 0) return true;
            }
        }
    }

    return false;
}

// 事件主循环
void monitor_loop(int fan_fd, int ipc_fd, const feb_config_t *config, volatile sig_atomic_t *running) {
    char buf[8192] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    ssize_t len;
    struct pollfd fds[1];

    fds[0].fd = fan_fd;
    fds[0].events = POLLIN;

    while (*running) {
        // 使用 poll 等待事件，避免 100% CPU 占用 (非阻塞读取)
        int ret = poll(fds, 1, 500); // 500ms 超时，允许检查 running 标志
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll failed");
            break;
        }
        if (ret == 0) continue; // 超时

        len = read(fan_fd, buf, sizeof(buf));
        if (len == -1) {
            if (errno == EAGAIN || errno == EINTR) continue;
            LOG_ERROR("Error reading fanotify event");
            break;
        }

        const struct fanotify_event_metadata *metadata = (struct fanotify_event_metadata *)buf;

        while (FAN_EVENT_OK(metadata, len)) {
            // 检查版本兼容性
            if (metadata->fd != FAN_NOFD) {
                feb_event_t event = {0};
                
                // 获取路径
                if (get_path_from_fd(metadata->fd, event.path, sizeof(event.path)) != 0) {
                    LOG_WARN("Failed to get path for fd %d, skipping event", metadata->fd);
                    close(metadata->fd);
                    metadata = FAN_EVENT_NEXT(metadata, len);
                    continue;
                }
                
                // 检查是否在排除列表中
                if (!should_skip(event.path, config)) {
                    // 获取文件元数据 (大小、时间)
                    struct stat st;
                    if (fstat(metadata->fd, &st) == 0) {
                        event.size = (uint64_t)st.st_size;
                        event.timestamp = (int64_t)st.st_mtim.tv_sec;
                    }
                    
                    event.mask = metadata->mask;

                    // 解析事件类型
                    if (metadata->mask & FAN_CLOSE_WRITE) {
                        event.event_type = FEB_EVENT_CLOSE_WRITE;
                    } else if (metadata->mask & FAN_MOVED_TO) {
                        event.event_type = FEB_EVENT_MOVED_TO;
                    } else if (metadata->mask & FAN_MOVED_FROM) {
                        event.event_type = FEB_EVENT_MOVED_FROM;
                    } else if (metadata->mask & FAN_CREATE) {
                        event.event_type = FEB_EVENT_CREATE;
                    } else if (metadata->mask & FAN_DELETE) {
                        event.event_type = FEB_EVENT_DELETE;
                    } else if (metadata->mask & FAN_MODIFY) {
                        event.event_type = FEB_EVENT_MODIFY;
                    }

                    // 分发给 IPC 模块
                    ipc_broadcast(ipc_fd, &event);
                    
                    LOG_DEBUG("Event sent: %s (Size: %lu)", event.path, event.size);
                }

                close(metadata->fd); // 必须手动关闭内核提供的 fd
            }
            metadata = FAN_EVENT_NEXT(metadata, len);
        }
    }
    
    // 清理 io_uring
    io_uring_cleanup(&g_io_uring);
}

void monitor_cleanup(int fan_fd) {
    if (fan_fd >= 0) {
        close(fan_fd);
    }
}