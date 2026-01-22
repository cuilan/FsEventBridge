#include "fseventbridge.h"
#include <sys/fanotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

// 初始化 fanotify 句柄
int monitor_init(const feb_config_t *config) {
    // 1. 初始化 fanotify 组
    // FAN_CLASS_NOTIF: 普通通知模式，不阻塞写操作
    // O_RDONLY | O_CLOEXEC: 句柄只读，且在 exec 时自动关闭
    int fan_fd = fanotify_init(FAN_CLASS_NOTIF, O_RDONLY | O_CLOEXEC);
    if (fan_fd == -1) {
        perror("fanotify_init 失败");
        return -1;
    }

    // 2. 设置监控标记 (递归监控的核心)
    // FAN_MARK_ADD: 添加新标记
    // FAN_MARK_FILESYSTEM: 监控整个文件系统 (实现递归的关键)
    // FAN_CLOSE_WRITE: 关键！只捕获文件写完并关闭的事件
    uint64_t mask = FAN_CLOSE_WRITE | FAN_EVENT_ON_CHILD;
    unsigned int flags = FAN_MARK_ADD | FAN_MARK_FILESYSTEM;

    if (fanotify_mark(fan_fd, flags, mask, AT_FDCWD, config->monitor_path) == -1) {
        perror("fanotify_mark 失败");
        close(fan_fd);
        return -1;
    }

    return fan_fd;
}

// 从文件描述符获取真实路径 (利用 /proc/self/fd)
static void get_path_from_fd(int fd, char *path, size_t size) {
    char procp[64];
    snprintf(procp, sizeof(procp), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(procp, path, size - 1);
    if (len != -1) {
        path[len] = '\0';
    } else {
        strncpy(path, "unknown", size);
    }
}

// 事件主循环
void monitor_loop(int fan_fd, const feb_config_t *config) {
    char buf[8192] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    ssize_t len;

    while (true) {
        len = read(fan_fd, buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            perror("读取 fanotify 事件错误");
            break;
        }

        const struct fanotify_event_metadata *metadata = (struct fanotify_event_metadata *)buf;

        while (FAN_EVENT_OK(metadata, len)) {
            // 检查版本兼容性
            if (metadata->fd != FAN_NOFD) {
                feb_event_t event = {0};
                
                // 获取路径
                get_path_from_fd(metadata->fd, event.path, sizeof(event.path));
                
                // 获取文件元数据 (大小、时间)
                struct stat st;
                if (fstat(metadata->fd, &st) == 0) {
                    event.size = (uint64_t)st.st_size;
                    event.timestamp = (int64_t)st.st_mtim.tv_sec;
                }
                
                event.mask = metadata->mask;

                // --- 处理逻辑 ---
                // 这里可以先进行初步过滤 (比如后缀检查)，然后分发给 IPC 模块
                printf("[DEBUG] 检测到文件写入完成: %s (Size: %lu)\n", event.path, event.size);
                
                // TODO: ipc_broadcast(server_fd, &event);

                close(metadata->fd); // 必须手动关闭内核提供的 fd
            }
            metadata = FAN_EVENT_NEXT(metadata, len);
        }
    }
}