#include "fseventbridge.h"
#include "toml.h"  // 假设已经包含 tomlc99 源码
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_default_config(feb_config_t *config) {
    memset(config, 0, sizeof(feb_config_t));
    strncpy(config->monitor_path, "/tmp", FEB_MAX_PATH - 1);
    strncpy(config->socket_path, "/tmp/feb.sock", FEB_MAX_PATH - 1);
    config->recursive = true;
    config->use_io_uring = true;
    config->log_level = FEB_LOG_INFO;
}

bool config_load(feb_config_t *config, const char *path) {
    set_default_config(config);
    if (path == NULL) return true;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("无法打开配置文件");
        return false;
    }

    char errbuf[200];
    toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!conf) {
        fprintf(stderr, "TOML 解析错误: %s\n", errbuf);
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
        
        // 解析排除列表 (Array)
        toml_array_t *excl = toml_array_in(monitor, "exclude_extensions");
        if (excl) {
            config->exclude_exts_count = toml_array_nelem(excl);
            config->exclude_exts = calloc(config->exclude_exts_count, sizeof(char*));
            for (int i = 0; i < config->exclude_exts_count; i++) {
                toml_datum_t ext = toml_string_at(excl, i);
                if (ext.ok) config->exclude_exts[i] = ext.u.s;
            }
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
}