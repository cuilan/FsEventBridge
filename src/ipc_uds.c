#include "fseventbridge.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>

#define INITIAL_CLIENTS 16
#define MAX_CLIENTS FEB_IPC_MAX_CLIENT_SLOTS
#define GROW_FACTOR 2

// 未在配置里指定 ipc_per_client_queue_max 时的默认队列上限（字节）
#define FEB_IPC_QUEUE_DEFAULT_MAX (256 * 1024)

typedef struct {
    int fd;                   // -1 表示空闲槽位
    unsigned char *pending;
    size_t pend_len;
    size_t pend_cap;
} ipc_slot_t;

static ipc_slot_t *slots = NULL;
static int slot_capacity = 0;
static int slot_connected = 0;

// 运行时配置快照（可为 NULL，表示全部走默认值）
static const feb_config_t *g_ipc_cfg;

static uint64_t g_stat_broadcasts;
static uint64_t g_stat_bytes_sent;
static uint64_t g_stat_disconnect_epipe;
static uint64_t g_stat_disconnect_queue_full;
static uint64_t g_stat_disconnect_fatal;
static uint64_t g_stat_eagain_queued;
static uint64_t g_stat_skip_events;
static uint64_t g_stat_discard_pending;

// 实际生效的每连接队列上限（字节）
static size_t ipc_effective_queue_max(void) {
    if (g_ipc_cfg && g_ipc_cfg->ipc_per_client_queue_max > 0)
        return g_ipc_cfg->ipc_per_client_queue_max;
    return FEB_IPC_QUEUE_DEFAULT_MAX;
}

static feb_ipc_queue_full_policy_t ipc_effective_policy(void) {
    if (g_ipc_cfg)
        return g_ipc_cfg->ipc_queue_full_policy;
    return FEB_IPC_QUEUE_FULL_DISCONNECT;
}

static void slot_free_queue(ipc_slot_t *s) {
    free(s->pending);
    s->pending = NULL;
    s->pend_len = 0;
    s->pend_cap = 0;
}

static void disconnect_slot(int idx, const char *reason) {
    ipc_slot_t *s = &slots[idx];
    if (s->fd < 0) return;
    LOG_INFO("[IPC] Client disconnected (%s), FD: %d", reason, s->fd);
    close(s->fd);
    s->fd = -1;
    slot_free_queue(s);
    if (slot_connected > 0) slot_connected--;
}

static void grow_slots(void) {
    if (slot_capacity >= MAX_CLIENTS) return;

    int new_cap = slot_capacity == 0 ? INITIAL_CLIENTS : slot_capacity * GROW_FACTOR;
    if (new_cap > MAX_CLIENTS) new_cap = MAX_CLIENTS;

    ipc_slot_t *n = realloc(slots, (size_t)new_cap * sizeof(ipc_slot_t));
    if (!n) return;

    for (int i = slot_capacity; i < new_cap; i++) {
        n[i].fd = -1;
        n[i].pending = NULL;
        n[i].pend_len = 0;
        n[i].pend_cap = 0;
    }
    slots = n;
    slot_capacity = new_cap;
    LOG_INFO("Client capacity grew to %d", new_cap);
}

static int queue_append(ipc_slot_t *s, const void *data, size_t len) {
    if (len == 0) return 0;
    size_t cap = ipc_effective_queue_max();
    if (s->pend_len + len > cap) return -1;
    if (s->pend_len + len > s->pend_cap) {
        size_t want = s->pend_cap ? s->pend_cap : 4096u;
        while (want < s->pend_len + len) want *= 2;
        unsigned char *p = realloc(s->pending, want);
        if (!p) return -1;
        s->pending = p;
        s->pend_cap = want;
    }
    memcpy(s->pending + s->pend_len, data, len);
    s->pend_len += len;
    return 0;
}

// 非阻塞写出 pending；返回 true 表示已写完；遇 EAGAIN 返回 false（等 POLLOUT 或下一轮 idle）
static bool flush_pending_slot(ipc_slot_t *s, int idx) {
    while (s->pend_len > 0) {
        ssize_t n = send(s->fd, s->pending, s->pend_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            g_stat_bytes_sent += (uint64_t)n;
            if ((size_t)n < s->pend_len) {
                memmove(s->pending, s->pending + (size_t)n, s->pend_len - (size_t)n);
            }
            s->pend_len -= (size_t)n;
            continue;
        }
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            if (errno == EPIPE || errno == ECONNRESET) {
                g_stat_disconnect_epipe++;
                disconnect_slot(idx, "broken pipe");
                return false;
            }
            g_stat_disconnect_fatal++;
            disconnect_slot(idx, "flush pending send error");
            return false;
        }
    }
    return true;
}

static void on_queue_append_failure(ipc_slot_t *s, int idx, const char *line, size_t line_len, bool after_discard);

// 向单个客户端发送一行 NDJSON（含末尾换行）；内部处理 EAGAIN 入队与队列满策略
static void slot_send_line_attempt(int idx, const char *line, size_t line_len, bool after_discard) {
    ipc_slot_t *s = &slots[idx];
    if (s->fd < 0) return;

    // 先尽量把旧积压刷掉，避免无限增长
    if (s->pend_len > 0) {
        flush_pending_slot(s, idx);
        if (s->fd < 0) return;
    }

    // 仍有积压说明对端暂时不可写，只能先排队
    if (s->pend_len > 0) {
        if (queue_append(s, line, line_len) != 0) {
            on_queue_append_failure(s, idx, line, line_len, after_discard);
            return;
        }
        g_stat_eagain_queued++;
        return;
    }

    const char *p = line;
    size_t left = line_len;
    while (left > 0) {
        ssize_t n = send(s->fd, p, left, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            g_stat_bytes_sent += (uint64_t)n;
            p += (size_t)n;
            left -= (size_t)n;
            continue;
        }
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (queue_append(s, p, left) != 0) {
                    on_queue_append_failure(s, idx, line, line_len, after_discard);
                    return;
                }
                g_stat_eagain_queued++;
                return;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                g_stat_disconnect_epipe++;
                disconnect_slot(idx, "broken pipe");
                return;
            }
            g_stat_disconnect_fatal++;
            disconnect_slot(idx, "send error");
            return;
        }
    }
}

// 入队失败：按配置决定断开、丢积压重试一次，或跳过本条事件
static void on_queue_append_failure(ipc_slot_t *s, int idx, const char *line, size_t line_len, bool after_discard) {
    feb_ipc_queue_full_policy_t pol = ipc_effective_policy();
    if (pol == FEB_IPC_QUEUE_FULL_DISCARD_PENDING && !after_discard) {
        g_stat_discard_pending++;
        slot_free_queue(s);
        slot_send_line_attempt(idx, line, line_len, true);
        return;
    }
    if (pol == FEB_IPC_QUEUE_FULL_SKIP_EVENT && !after_discard) {
        g_stat_skip_events++;
        return;
    }
    g_stat_disconnect_queue_full++;
    disconnect_slot(idx, "per-client send queue full");
}

static void slot_send_line(int idx, const char *line, size_t line_len) {
    size_t cap = ipc_effective_queue_max();
    // 单行 NDJSON 超过上限时永远无法入队，直接断开以免策略死循环
    if (line_len > cap) {
        g_stat_disconnect_queue_full++;
        disconnect_slot(idx, "NDJSON line larger than per-client queue cap");
        return;
    }
    slot_send_line_attempt(idx, line, line_len, false);
}

static void maybe_log_ipc_stats(void) {
    if (g_stat_broadcasts > 0 && g_stat_broadcasts % 2000 == 0) {
        LOG_INFO("[IPC] stats broadcasts=%" PRIu64 " clients=%d sent_bytes=%" PRIu64
                 " disc_epipe=%" PRIu64 " disc_queue_full=%" PRIu64 " disc_other=%" PRIu64
                 " skip_events=%" PRIu64 " discard_pending=%" PRIu64,
                 g_stat_broadcasts,
                 slot_connected,
                 g_stat_bytes_sent,
                 g_stat_disconnect_epipe,
                 g_stat_disconnect_queue_full,
                 g_stat_disconnect_fatal,
                 g_stat_skip_events,
                 g_stat_discard_pending);
    }
}

int ipc_init(const char *socket_path, const feb_config_t *config) {
    g_ipc_cfg = config;

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        LOG_ERROR("Socket creation failed");
        return -1;
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    unlink(socket_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("Socket binding failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 64) == -1) {
        LOG_ERROR("Socket listening failed");
        close(server_fd);
        return -1;
    }

    grow_slots();

    return server_fd;
}

void ipc_accept_pending(int server_fd) {
    while (true) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Accept error");
            }
            break;
        }

        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        }

        grow_slots();

        bool added = false;
        for (int i = 0; i < slot_capacity; i++) {
            if (slots[i].fd == -1) {
                slots[i].fd = cfd;
                slots[i].pend_len = 0;
                slot_connected++;
                added = true;
                LOG_INFO("[IPC] New client connected, FD: %d (active=%d)", cfd, slot_connected);
                break;
            }
        }

        if (!added) {
            LOG_WARN("[IPC] Client limit reached (%d), rejecting connection", slot_capacity);
            close(cfd);
        }
    }
}

void ipc_idle_flush(void) {
    for (int i = 0; i < slot_capacity; i++) {
        ipc_slot_t *s = &slots[i];
        if (s->fd < 0 || s->pend_len == 0) continue;
        flush_pending_slot(s, i);
    }
}

int ipc_append_pollout_fds(struct pollfd *pf_base, int *slot_index_out, int max_pairs) {
    int n = 0;
    for (int i = 0; i < slot_capacity && n < max_pairs; i++) {
        ipc_slot_t *s = &slots[i];
        if (s->fd >= 0 && s->pend_len > 0) {
            pf_base[n].fd = s->fd;
            pf_base[n].events = POLLOUT;
            pf_base[n].revents = 0;
            slot_index_out[n] = i;
            n++;
        }
    }
    return n;
}

void ipc_dispatch_pollout_revents(struct pollfd *pf_base, const int *slot_index_in, int n_pairs) {
    for (int k = 0; k < n_pairs; k++) {
        if (pf_base[k].revents & (POLLOUT | POLLERR | POLLHUP)) {
            int idx = slot_index_in[k];
            ipc_slot_t *s = &slots[idx];
            if (s->fd >= 0 && s->pend_len > 0)
                flush_pending_slot(s, idx);
        }
    }
}

void ipc_broadcast(int server_fd, const feb_event_t *event) {
    (void)server_fd;

    char json_buf[4096];
    json_writer_t w;

    json_init(&w, json_buf, sizeof(json_buf) - 2);

    json_start_object(&w);
    json_key_string(&w, "path", event->path);
    json_key_string(&w, "event", feb_event_name(event->event_type));
    json_key_int(&w, "type", (int64_t)event->event_type);
    json_key_uint(&w, "size", event->size);
    json_key_int(&w, "ts", event->ts);
    json_key_int(&w, "mtime", event->mtime);
    json_key_uint(&w, "mask", event->mask);
    json_end_object(&w);

    if (w.error) {
        LOG_WARN("JSON buffer overflow or encoding error");
        return;
    }

    size_t current_len = w.offset;
    json_buf[current_len++] = '\n';
    json_buf[current_len] = '\0';
    int json_len = (int)current_len;

    g_stat_broadcasts++;

    for (int i = 0; i < slot_capacity; i++) {
        if (slots[i].fd == -1) continue;
        slot_send_line(i, json_buf, (size_t)json_len);
    }

    maybe_log_ipc_stats();
}

void ipc_cleanup(int server_fd, const char *socket_path) {
    g_ipc_cfg = NULL;
    if (slots) {
        for (int i = 0; i < slot_capacity; i++) {
            if (slots[i].fd >= 0) {
                close(slots[i].fd);
                slots[i].fd = -1;
            }
            slot_free_queue(&slots[i]);
        }
        free(slots);
        slots = NULL;
        slot_capacity = 0;
        slot_connected = 0;
    }
    close(server_fd);
    unlink(socket_path);
}
