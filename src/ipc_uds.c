#include "fseventbridge.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_CLIENTS 16

// 内部结构：管理连接的客户端
static struct {
    int fds[MAX_CLIENTS];
    int count;
} clients = {.count = 0};

// 初始化 Unix Domain Socket 服务器
int ipc_init(const char *socket_path) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket 创建失败");
        return -1;
    }

    // 设置非阻塞模式
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    // 如果 Socket 文件已存在，先删除它
    unlink(socket_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("Socket 绑定失败");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) == -1) {
        perror("Socket 监听失败");
        close(server_fd);
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) clients.fds[i] = -1;

    return server_fd;
}

// 处理新的连接请求
void ipc_accept_clients(int server_fd) {
    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Accept 错误");
            }
            break;
        }

        // 将新客户端加入列表
        bool added = false;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients.fds[i] == -1) {
                clients.fds[i] = client_fd;
                clients.count++;
                added = true;
                printf("[IPC] 新客户端连接，FD: %d\n", client_fd);
                break;
            }
        }

        if (!added) {
            printf("[IPC] 客户端已满，拒绝连接\n");
            close(client_fd);
        }
    }
}

// 向所有客户端广播 JSON 事件
void ipc_broadcast(int server_fd, const feb_event_t *event) {
    // 1. 先尝试接受可能的并发连接
    ipc_accept_clients(server_fd);

    // 2. 构造 JSON 字符串 (NDJSON 格式)
    char json_buf[1024];
    int json_len = snprintf(json_buf, sizeof(json_buf),
        "{\"path\":\"%s\",\"size\":%lu,\"ts\":%ld,\"mask\":%u}\n",
        event->path, event->size, event->timestamp, event->mask);

    if (json_len < 0 || json_len >= (int)sizeof(json_buf)) return;

    // 3. 遍历客户端发送数据
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = clients.fds[i];
        if (fd == -1) continue;

        // MSG_NOSIGNAL 标志，处理 破碎管道 (EPIPE)
        ssize_t sent = send(fd, json_buf, json_len, MSG_NOSIGNAL);
        if (sent == -1) {
            // 如果上游程序关闭了连接，清理 FD
            if (errno == EPIPE || errno == ECONNRESET) {
                printf("[IPC] 客户端断开，清理 FD: %d\n", fd);
                close(fd);
                clients.fds[i] = -1;
                clients.count--;
            }
        }
    }
}

void ipc_cleanup(int server_fd, const char *socket_path) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients.fds[i] != -1) close(clients.fds[i]);
    }
    close(server_fd);
    unlink(socket_path);
}