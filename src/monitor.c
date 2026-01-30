#include "fseventbridge.h"
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <unistd.h>

// 初始化 fanotify 句柄
int monitor_init(const feb_config_t *config) {
  // 1. 初始化 fanotify 组
  // FAN_CLASS_NOTIF: 普通通知模式，不阻塞写操作
  // O_RDONLY | O_CLOEXEC: 句柄只读，且在 exec 时自动关闭
  int fan_fd =
      fanotify_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY | O_CLOEXEC);
  if (fan_fd == -1) {
    LOG_ERROR("fanotify_init failed");
    return -1;
  }

  // 2. 设置监控标记 (递归监控的核心)
  // FAN_MARK_ADD: 添加新标记
  // FAN_MARK_FILESYSTEM: 监控整个文件系统 (实现递归的关键)
  // FAN_CLOSE_WRITE: 关键！只捕获文件写完并关闭的事件
  // uint64_t mask = FAN_CLOSE_WRITE | FAN_EVENT_ON_CHILD;
  uint64_t mask = FAN_CLOSE_WRITE; // 对于 FAN_MARK_FILESYSTEM, 不需要 ON_CHILD
  unsigned int flags = FAN_MARK_ADD | FAN_MARK_FILESYSTEM;

  if (fanotify_mark(fan_fd, flags, mask, AT_FDCWD, config->monitor_path) ==
      -1) {
    LOG_ERROR("fanotify_mark failed");
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

// 简单的后缀名过滤
static bool should_skip(const char *path, const feb_config_t *config) {
  if (config->exclude_exts_count == 0)
    return false;

  const char *dot = strrchr(path, '.');
  if (!dot)
    return false;

  for (int i = 0; i < config->exclude_exts_count; i++) {
    if (strcmp(dot, config->exclude_exts[i]) == 0)
      return true;
  }
  return false;
}

static void process_fanotify_events(int ipc_fd, const feb_config_t *config,
                                    char *buf, ssize_t len) {
  const struct fanotify_event_metadata *metadata =
      (struct fanotify_event_metadata *)buf;

  while (FAN_EVENT_OK(metadata, len)) {
    // 检查版本兼容性
    if (metadata->fd != FAN_NOFD) {
      feb_event_t event = {0};

      // 获取路径
      get_path_from_fd(metadata->fd, event.path, sizeof(event.path));

      // 检查是否在排除列表中
      if (!should_skip(event.path, config)) {
        // 获取文件元数据 (大小、时间)
        struct stat st;
        if (fstat(metadata->fd, &st) == 0) {
          event.size = (uint64_t)st.st_size;
          event.timestamp = (int64_t)st.st_mtim.tv_sec;
        }

        event.mask = metadata->mask;

        // 分发给 IPC 模块
        ipc_broadcast(ipc_fd, &event);

        LOG_DEBUG("Event sent: %s (Size: %lu)", event.path, event.size);
      }

      close(metadata->fd); // 必须手动关闭内核提供的 fd
    }
    metadata = FAN_EVENT_NEXT(metadata, len);
  }
}

static void monitor_loop_poll(int fan_fd, int ipc_fd,
                              const feb_config_t *config,
                              volatile sig_atomic_t *running) {
  char buf[8192]
      __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
  ssize_t len;
  struct pollfd fds[1];

  fds[0].fd = fan_fd;
  fds[0].events = POLLIN;

  LOG_INFO("Using poll() for event loop");

  while (*running) {
    // 使用 poll 等待事件，避免 100% CPU 占用 (非阻塞读取)
    int ret = poll(fds, 1, 500); // 500ms 超时，允许检查 running 标志
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      LOG_ERROR("poll failed");
      break;
    }
    if (ret == 0)
      continue; // 超时

    len = read(fan_fd, buf, sizeof(buf));
    if (len == -1) {
      if (errno == EAGAIN || errno == EINTR)
        continue;
      LOG_ERROR("Error reading fanotify event");
      break;
    }

    process_fanotify_events(ipc_fd, config, buf, len);
  }
}

static void monitor_loop_io_uring(int fan_fd, int ipc_fd,
                                  const feb_config_t *config,
                                  volatile sig_atomic_t *running) {
  struct io_uring ring;
  // 队列深度 16 足够单个 fd 监控
  int ret = io_uring_queue_init(16, &ring, 0);
  if (ret < 0) {
    LOG_ERROR("io_uring_queue_init failed: %s, falling back to poll",
              strerror(-ret));
    monitor_loop_poll(fan_fd, ipc_fd, config, running);
    return;
  }

  LOG_INFO("Using io_uring for event loop");

  char buf[8192]
      __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
  struct io_uring_sqe *sqe;
  struct io_uring_cqe *cqe;

  // 提交第一个读请求
  sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    LOG_ERROR("io_uring_get_sqe failed");
    io_uring_queue_exit(&ring);
    return;
  }
  io_uring_prep_read(sqe, fan_fd, buf, sizeof(buf), 0);
  io_uring_submit(&ring);

  while (*running) {
    struct __kernel_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 500 * 1000000; // 500ms

    // 等待 CQE
    ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);

    if (ret == -ETIME) {
      continue;
    } else if (ret < 0) {
      if (ret == -EINTR)
        continue;
      LOG_ERROR("io_uring_wait_cqe_timeout failed: %s", strerror(-ret));
      break;
    }

    if (cqe->res < 0) {
      if (cqe->res == -EAGAIN || cqe->res == -EINTR) {
        // 只是被信号打断或无数据，重新提交即可
      } else {
        LOG_ERROR("Async read failed: %s", strerror(-cqe->res));
        io_uring_cqe_seen(&ring, cqe);
        break;
      }
    } else if (cqe->res > 0) {
      process_fanotify_events(ipc_fd, config, buf, cqe->res);
    }

    // 标记 CQE 为已处理
    io_uring_cqe_seen(&ring, cqe);

    // 重新提交读请求
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      LOG_ERROR("io_uring_get_sqe failed during loop");
      break;
    }
    io_uring_prep_read(sqe, fan_fd, buf, sizeof(buf), 0);
    io_uring_submit(&ring);
  }

  io_uring_queue_exit(&ring);
}

// 事件主循环
void monitor_loop(int fan_fd, int ipc_fd, const feb_config_t *config,
                  volatile sig_atomic_t *running) {
  if (config->use_io_uring) {
    monitor_loop_io_uring(fan_fd, ipc_fd, config, running);
  } else {
    monitor_loop_poll(fan_fd, ipc_fd, config, running);
  }
}

void monitor_cleanup(int fan_fd) {
  if (fan_fd >= 0) {
    close(fan_fd);
  }
}
