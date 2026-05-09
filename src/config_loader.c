#include "fseventbridge.h"
#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/fanotify.h>

static void set_default_config(feb_config_t *config) {
    memset(config, 0, sizeof(feb_config_t));
    strncpy(config->socket_path, "/tmp/feb.sock", FEB_MAX_PATH - 1);
    config->recursive = true;
    config->use_io_uring = true;
    config->log_level = FEB_LOG_INFO;
    config->event_mask = FAN_CLOSE_WRITE;
    // ipc_per_client_queue_max == 0 表示使用内置默认（见 ipc_uds 中 ipc_effective_queue_max）
    config->ipc_queue_full_policy = FEB_IPC_QUEUE_FULL_DISCONNECT;
}

// 解析 [ipc] on_queue_full；无法识别时返回 false
static bool parse_ipc_queue_full_policy(const char *s, feb_ipc_queue_full_policy_t *out) {
    if (!s) return false;
    if (strcmp(s, "disconnect") == 0) {
        *out = FEB_IPC_QUEUE_FULL_DISCONNECT;
        return true;
    }
    if (strcmp(s, "discard_pending") == 0) {
        *out = FEB_IPC_QUEUE_FULL_DISCARD_PENDING;
        return true;
    }
    if (strcmp(s, "skip_event") == 0) {
        *out = FEB_IPC_QUEUE_FULL_SKIP_EVENT;
        return true;
    }
    return false;
}

// 将 TOML 字符串数组拷贝到 (out_arr, out_count)；自动跳过空字符串
// 调用方需保证 out_arr/out_count 当前为空（NULL/0）
static void load_string_array(toml_array_t *arr, char ***out_arr, int *out_count) {
    if (arr == NULL) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;

    char **buf = calloc((size_t)n, sizeof(char *));
    if (!buf) return;

    int real = 0;
    for (int i = 0; i < n; i++) {
        toml_datum_t v = toml_string_at(arr, i);
        if (!v.ok) continue;
        if (v.u.s == NULL || v.u.s[0] == '\0') {
            free(v.u.s);
            continue;
        }
        buf[real++] = v.u.s;
    }

    if (real == 0) {
        free(buf);
        return;
    }

    *out_arr = buf;
    *out_count = real;
}

// 把日志级别字符串映射到枚举；非法值返回 false（保持原值不变）
static bool parse_log_level(const char *s, feb_log_level_t *out) {
    if (!s) return false;
    if (strcmp(s, "debug") == 0) { *out = FEB_LOG_DEBUG; return true; }
    if (strcmp(s, "info")  == 0) { *out = FEB_LOG_INFO;  return true; }
    if (strcmp(s, "warn")  == 0) { *out = FEB_LOG_WARN;  return true; }
    if (strcmp(s, "error") == 0) { *out = FEB_LOG_ERROR; return true; }
    return false;
}

bool config_load(feb_config_t *config, const char *path) {
    // 设置默认配置
    set_default_config(config);
    if (path == NULL) return true;

    // 打开配置文件
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("Error: Failed to open configuration file");
        return false;
    }

    // 解析配置文件
    char errbuf[200];
    toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!conf) {
        fprintf(stderr, "Error: TOML parsing error: %s\n", errbuf);
        return false;
    }

    // 解析 [server] 部分
    toml_table_t *server = toml_table_in(conf, "server");
    if (server) {
        toml_datum_t sock = toml_string_in(server, "socket_path");
        if (sock.ok) {
            strncpy(config->socket_path, sock.u.s, FEB_MAX_PATH - 1);
            config->socket_path[FEB_MAX_PATH - 1] = '\0';
            free(sock.u.s);
        }

        // 日志级别（可选；CLI -l 仍可覆盖此值）
        toml_datum_t lvl = toml_string_in(server, "log_level");
        if (lvl.ok) {
            if (!parse_log_level(lvl.u.s, &config->log_level)) {
                fprintf(stderr,
                        "Warning: unknown log_level '%s' in [server], "
                        "expected one of debug/info/warn/error\n",
                        lvl.u.s);
            }
            free(lvl.u.s);
        }
    }

    // 解析 [monitor] 部分
    toml_table_t *monitor = toml_table_in(conf, "monitor");
    if (monitor) {
        toml_datum_t mpath = toml_string_in(monitor, "path");
        if (mpath.ok) {
            strncpy(config->monitor_path, mpath.u.s, FEB_MAX_PATH - 1);
            config->monitor_path[FEB_MAX_PATH - 1] = '\0';
            free(mpath.u.s);
        }

        toml_datum_t recur = toml_bool_in(monitor, "recursive");
        if (recur.ok) config->recursive = recur.u.b;

        // 排除扩展名列表
        toml_array_t *excl = toml_array_in(monitor, "exclude_extensions");
        load_string_array(excl, &config->exclude_exts, &config->exclude_exts_count);

        // 排除路径列表：优先 exclude_paths（推荐），否则尝试 exclude_path（兼容旧示例）
        toml_array_t *excl_paths = toml_array_in(monitor, "exclude_paths");
        if (excl_paths == NULL) {
            excl_paths = toml_array_in(monitor, "exclude_path");
        }
        load_string_array(excl_paths, &config->exclude_paths, &config->exclude_paths_count);

        // 解析事件列表
        toml_array_t *events = toml_array_in(monitor, "events");
        if (events) {
            config->event_mask = 0;
            int event_count = toml_array_nelem(events);
            for (int i = 0; i < event_count; i++) {
                toml_datum_t evt = toml_string_at(events, i);
                if (evt.ok) {
                    if (strcmp(evt.u.s, "CLOSE_WRITE") == 0) config->event_mask |= FAN_CLOSE_WRITE;
                    else if (strcmp(evt.u.s, "MOVED_TO") == 0) config->event_mask |= FAN_MOVED_TO;
                    else if (strcmp(evt.u.s, "MOVED_FROM") == 0) config->event_mask |= FAN_MOVED_FROM;
                    else if (strcmp(evt.u.s, "CREATE") == 0) config->event_mask |= FAN_CREATE;
                    else if (strcmp(evt.u.s, "DELETE") == 0) config->event_mask |= FAN_DELETE;
                    else if (strcmp(evt.u.s, "MODIFY") == 0) config->event_mask |= FAN_MODIFY;
                    free(evt.u.s);
                }
            }
            // 没有任何识别到的事件名，回退到默认值，避免 mark 失败
            if (config->event_mask == 0) {
                config->event_mask = FAN_CLOSE_WRITE;
            }
        }
    }

    // 解析 [processor] 部分（目前仅 use_io_uring 生效；worker_threads 等保留为未来字段）
    toml_table_t *processor = toml_table_in(conf, "processor");
    if (processor) {
        toml_datum_t use_uring = toml_bool_in(processor, "use_io_uring");
        if (use_uring.ok) config->use_io_uring = use_uring.u.b;
    }

    // [ipc]：慢客户端时每连接待发队列字节上限与触顶策略
    toml_table_t *ipc = toml_table_in(conf, "ipc");
    if (ipc) {
        toml_datum_t qmax = toml_int_in(ipc, "per_client_queue_max_bytes");
        if (qmax.ok) {
            if (qmax.u.i < 0) {
                fprintf(stderr,
                        "Warning: per_client_queue_max_bytes must be non-negative "
                        "(got %" PRId64 "), using built-in default\n",
                        qmax.u.i);
            } else {
                config->ipc_per_client_queue_max = (size_t)qmax.u.i;
            }
        }

        toml_datum_t pol = toml_string_in(ipc, "on_queue_full");
        if (pol.ok) {
            if (!parse_ipc_queue_full_policy(pol.u.s, &config->ipc_queue_full_policy)) {
                fprintf(stderr,
                        "Warning: unknown on_queue_full '%s' in [ipc], "
                        "expected disconnect / discard_pending / skip_event\n",
                        pol.u.s);
            }
            free(pol.u.s);
        }
    }

    toml_free(conf);
    return true;
}

void config_destroy(feb_config_t *config) {
    if (config->exclude_exts) {
        for (int i = 0; i < config->exclude_exts_count; i++) {
            free(config->exclude_exts[i]);
        }
        free(config->exclude_exts);
    }
    if (config->exclude_paths) {
        for (int i = 0; i < config->exclude_paths_count; i++) {
            free(config->exclude_paths[i]);
        }
        free(config->exclude_paths);
    }
}