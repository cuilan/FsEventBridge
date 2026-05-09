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
#include <time.h>
#include <inttypes.h>
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

    LOG_INFO("fanotify: FAN_MARK_FILESYSTEM on filesystem containing \"%s\"; forwarding only logical scope under anchor (recursive=%s).",
             config->monitor_path,
             config->recursive ? "true (full subtree below anchor)" : "false (direct children only)");

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

// 判断事件路径是否落在「逻辑监控范围」内（与 fanotify 使用 FAN_MARK_FILESYSTEM 的配合说明见 README）
static bool event_path_in_scope(const char *path, const feb_config_t *config) {
    const char *root = config->monitor_path;
    if (root == NULL || root[0] == '\0')
        return true;

    char norm[FEB_MAX_PATH];
    size_t n = strlen(root);
    if (n >= sizeof(norm))
        return false;
    memcpy(norm, root, n + 1);
    while (n > 1 && norm[n - 1] == '/')
        norm[--n] = '\0';

    // 锚点为根 "/"：凡是绝对路径再按 recursive 切深度
    if (norm[0] == '/' && norm[1] == '\0') {
        if (path[0] != '/')
            return false;
        if (config->recursive)
            return true;
        const char *rest = path + 1;
        return strchr(rest, '/') == NULL;
    }

    if (strcmp(path, norm) == 0)
        return true;
    if (!path_under_prefix(path, norm))
        return false;
    if (config->recursive)
        return true;

    const char *rest = path + strlen(norm);
    if (*rest == '/')
        rest++;
    return strchr(rest, '/') == NULL;
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

// 事件主循环：poll 多路复用——fanotify 读事件、监听 socket accept、以及对慢客户端追加 POLLOUT 写触发
void monitor_loop(int fan_fd, int ipc_fd, const feb_config_t *config, volatile sig_atomic_t *running) {
    char buf[8192] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    ssize_t len;

    while (*running) {
        struct pollfd fds[2 + FEB_IPC_MAX_CLIENT_SLOTS];
        int slot_idx[FEB_IPC_MAX_CLIENT_SLOTS];

        fds[0].fd = fan_fd;
        fds[0].events = POLLIN;
        fds[1].fd = ipc_fd;
        fds[1].events = POLLIN;

        // 仅有 pending 待发字节的客户端才挂 POLLOUT，避免在无积压时空转
        int n_pollout = ipc_append_pollout_fds(&fds[2], slot_idx, FEB_IPC_MAX_CLIENT_SLOTS);
        int nfds = 2 + n_pollout;

        int ret = poll(fds, (nfds_t)nfds, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll failed");
            break;
        }

        // 先有连接再谈写：accept 先于 POLLOUT 处理顺序并无强制要求，但先接新客户端更直观
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            ipc_accept_pending(ipc_fd);
        }

        // 内核通知可写时再尝试排空各客户端 backlog（与每轮末尾 ipc_idle_flush 互为补充）
        ipc_dispatch_pollout_revents(&fds[2], slot_idx, n_pollout);

        if (fds[0].revents & POLLIN) {
            len = read(fan_fd, buf, sizeof(buf));
            if (len == -1) {
                if (errno == EAGAIN || errno == EINTR) {
                    ipc_idle_flush();
                    continue;
                }
                LOG_ERROR("Error reading fanotify event");
                break;
            }

            const struct fanotify_event_metadata *metadata = (struct fanotify_event_metadata *)buf;

            while (FAN_EVENT_OK(metadata, len)) {
                if (metadata->fd != FAN_NOFD) {
                    feb_event_t event = {0};

                    if (get_path_from_fd(metadata->fd, event.path, sizeof(event.path)) != 0) {
                        LOG_WARN("Failed to get path for fd %d, skipping event", metadata->fd);
                        close(metadata->fd);
                        metadata = FAN_EVENT_NEXT(metadata, len);
                        continue;
                    }

                    if (event_path_in_scope(event.path, config) && !should_skip(event.path, config)) {
                        event.event_type = FEB_EVENT_UNKNOWN;
                        event.mtime = -1;

                        struct stat st;
                        if (fstat(metadata->fd, &st) == 0) {
                            event.size = (uint64_t)st.st_size;
                            event.mtime = (int64_t)st.st_mtim.tv_sec;
                        }

                        event.mask = metadata->mask;

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

                        struct timespec rt;
                        if (clock_gettime(CLOCK_REALTIME, &rt) == 0) {
                            event.ts = (int64_t)rt.tv_sec;
                        } else {
                            event.ts = (int64_t)time(NULL);
                        }

                        ipc_broadcast(ipc_fd, &event);

                        LOG_DEBUG("Event sent: %s event=%s type=%d ts=%" PRId64 " mtime=%" PRId64 " mask=0x%x size=%" PRIu64,
                                  event.path,
                                  feb_event_name(event.event_type),
                                  (int)event.event_type,
                                  event.ts,
                                  event.mtime,
                                  (unsigned)event.mask,
                                  event.size);
                    }

                    close(metadata->fd);
                }
                metadata = FAN_EVENT_NEXT(metadata, len);
            }
        }

        ipc_idle_flush();
    }

    io_uring_cleanup(&g_io_uring);
}

void monitor_cleanup(int fan_fd) {
    if (fan_fd >= 0) {
        close(fan_fd);
    }
}