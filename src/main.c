#include "fseventbridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <sys/fanotify.h>
#include <systemd/sd-daemon.h>

// 默认日志级别
feb_log_level_t g_log_level = FEB_LOG_INFO; 

// 全局运行标志，sig_atomic_t 确保信号处理的原子性
static volatile sig_atomic_t running = 1;

// 信号处理函数：捕捉 Ctrl+C 或 systemctl stop
static void signal_handler(int sig) {
    (void)sig;   // 防止编译器报“未使用的参数”警告
    running = 0; // 将运行标志置为 0，通知主循环停止
}

// 辅助函数：添加字符串到数组
static void add_string_to_array(char ***array, int *count, const char *str) {
    char **new_array = realloc(*array, sizeof(char *) * (*count + 1));
    if (!new_array) {
        LOG_ERROR("Memory allocation failed");
        exit(1);
    }
    *array = new_array;
    (*array)[*count] = strdup(str);
    (*count)++;
}

// 辅助函数：释放字符串数组
static void free_string_array(char ***array, int *count) {
    if (*array == NULL) return;
    for (int i = 0; i < *count; i++) {
        free((*array)[i]);
    }
    free(*array);
    *array = NULL;
    *count = 0;
}

// 把日志级别映射为可读字符串
static const char *log_level_str(feb_log_level_t lvl) {
    switch (lvl) {
        case FEB_LOG_DEBUG: return "debug";
        case FEB_LOG_INFO:  return "info";
        case FEB_LOG_WARN:  return "warn";
        case FEB_LOG_ERROR: return "error";
        default:            return "unknown";
    }
}

// 把字符串数组按逗号拼接到 stdout，用于 dry-run 输出
static void print_string_array(char **arr, int count) {
    for (int i = 0; i < count; i++) {
        if (i > 0) fputc(',', stdout);
        fputs(arr[i], stdout);
    }
    fputc('\n', stdout);
}

// 把 fanotify event_mask 解码为可读名称数组（用于 dry-run 输出）
static void print_event_names(uint32_t mask) {
    bool first = true;
    struct {
        uint32_t bit;
        const char *name;
    } table[] = {
        { FAN_CLOSE_WRITE, "CLOSE_WRITE" },
        { FAN_MODIFY,      "MODIFY"      },
        { FAN_MOVED_TO,    "MOVED_TO"    },
        { FAN_MOVED_FROM,  "MOVED_FROM"  },
        { FAN_CREATE,      "CREATE"      },
        { FAN_DELETE,      "DELETE"      },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (mask & table[i].bit) {
            if (!first) fputc(',', stdout);
            fputs(table[i].name, stdout);
            first = false;
        }
    }
    fputc('\n', stdout);
}

// 打印一条配置项：IPC 队列上限的最终生效值（0 的配置表示走内置默认）
static void print_resolved_config(const feb_config_t *config) {
    size_t ipc_q_eff = config->ipc_per_client_queue_max;
    if (ipc_q_eff == 0)
        ipc_q_eff = 256 * 1024;

    const char *ipc_pol = "disconnect";
    if (config->ipc_queue_full_policy == FEB_IPC_QUEUE_FULL_DISCARD_PENDING)
        ipc_pol = "discard_pending";
    else if (config->ipc_queue_full_policy == FEB_IPC_QUEUE_FULL_SKIP_EVENT)
        ipc_pol = "skip_event";

    printf("monitor_path=%s\n",  config->monitor_path);
    printf("socket_path=%s\n",   config->socket_path);
    printf("recursive=%s\n",     config->recursive ? "true" : "false");
    printf("use_io_uring=%s\n",  config->use_io_uring ? "true" : "false");
    printf("log_level=%s\n",     log_level_str(config->log_level));
    printf("event_mask=0x%x\n",  config->event_mask);
    printf("events=");
    print_event_names(config->event_mask);
    printf("exclude_extensions=");
    print_string_array(config->exclude_exts, config->exclude_exts_count);
    printf("exclude_paths=");
    print_string_array(config->exclude_paths, config->exclude_paths_count);
    printf("ipc_per_client_queue_max=%zu\n", ipc_q_eff);
    printf("ipc_on_queue_full=%s\n", ipc_pol);
}

// 帮助正文可用 FsEventBridge 作项目称谓；真正的命令名与安装一致为 fseventbridge（及 feb）
void print_usage(const char *prog_name) {
    printf("\n");
    printf("  ███████╗███████╗███████╗██╗   ██╗███████╗███╗   ██╗████████╗██████╗ ██████╗ ██╗██████╗  ██████╗ ███████╗\n");
    printf("  ██╔════╝██╔════╝██╔════╝██║   ██║██╔════╝████╗  ██║╚══██╔══╝██╔══██╗██╔══██╗██║██╔══██╗██╔════╝ ██╔════╝\n");
    printf("  █████╗  ███████╗█████╗  ██║   ██║█████╗  ██╔██╗ ██║   ██║   ██████╔╝██████╔╝██║██║  ██║██║  ███╗█████╗  \n");
    printf("  ██╔══╝  ╚════██║██╔══╝  ╚██╗ ██╔╝██╔══╝  ██║╚██╗██║   ██║   ██╔══██╗██╔══██╗██║██║  ██║██║   ██║██╔══╝  \n");
    printf("  ██║     ███████║███████╗ ╚████╔╝ ███████╗██║ ╚████║   ██║   ██████╔╝██║  ██║██║██████╔╝╚██████╔╝███████╗\n");
    printf("  ╚═╝     ╚══════╝╚══════╝  ╚═══╝  ╚══════╝╚═╝  ╚═══╝   ╚═╝   ╚═════╝ ╚═╝  ╚═╝╚═╝╚═════╝  ╚═════╝ ╚══════╝\n");
    printf("\n");
    printf("  High-Performance File System Event Bridge — FsEventBridge\n");
    printf("  Version: %s\n", FEB_VERSION);
    printf("  Command (installed): fseventbridge   Short name: feb (symlink when packaged)\n");
    printf("  Repo: https://github.com/cuilan/FsEventBridge\n\n");

    printf("Usage:\n");
    printf("  %s [options]\n", prog_name);
    printf("  feb [options]        (same program when the feb symlink is installed)\n\n");

    printf("Examples:\n");
    printf("  1. Monitor a directory recursively:\n");
    printf("     fseventbridge -d /data/logs -r\n\n");
    printf("  2. Monitor with config file:\n");
    printf("     fseventbridge -c /etc/fseventbridge/config.toml\n\n");
    printf("  3. Advanced usage (exclude extensions & paths, debug level):\n");
    printf("     fseventbridge -d /var/www -r -l debug -e .swp -e .tmp -x /var/www/cache\n\n");

    printf("Options:\n");
    printf("  -c, --config PATH    Specify the TOML configuration file path\n");
    printf("  -d, --dir PATH       Specify the monitoring directory (override configuration)\n");
    printf("  -s, --socket PATH    Specify the Unix Socket path (default: /tmp/feb.sock)\n");
    printf("  -r, --recursive      Enable recursive monitoring (default: false)\n");
    printf("  -i, --io-uring       Enable io_uring optimization (default: true)\n");
    printf("      --no-io-uring    Disable io_uring optimization (overrides config)\n");
    printf("  -l, --log-level      Specify the log level (debug, info, warn, error)\n");
    printf("  -e, --exclude-ext    Specify the exclude extension (multiple can be specified)\n");
    printf("  -x, --exclude-path   Specify the exclude path (multiple can be specified)\n");
    printf("      --check-config   Print resolved configuration and exit (no monitoring)\n");
    printf("  -v, --version        Display version information\n");
    printf("  -h, --help           Display this help information\n\n");
}

int main(int argc, char **argv) {
    feb_config_t config;
    char *config_file = NULL;
    int opt;

    // 1. 定义长选项
    // 仅长参数选项使用 256 起的虚拟短码，避免与 ASCII 短选项冲突
    enum {
        OPT_NO_IO_URING = 0x100,
        OPT_CHECK_CONFIG
    };

    bool check_config = false;

    static struct option long_options[] = {
        {"config",       required_argument, 0, 'c'},
        {"dir",          required_argument, 0, 'd'},
        {"socket",       required_argument, 0, 's'},
        {"recursive",    no_argument,       0, 'r'},
        {"io-uring",     no_argument,       0, 'i'},
        {"no-io-uring",  no_argument,       0, OPT_NO_IO_URING},
        {"log-level",    required_argument, 0, 'l'},
        {"exclude-ext",  required_argument, 0, 'e'},
        {"exclude-path", required_argument, 0, 'x'},
        {"check-config", no_argument,       0, OPT_CHECK_CONFIG},
        {"version",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    // 2. 解析命令行参数 (第一遍：获取配置文件路径)
    // 注意：optstring 中需要包含所有可能的选项字符，即使在这一遍我们只关心 -c
    // 为了避免 getopt_long 报错，需要提供完整的 optstring
    while ((opt = getopt_long(argc, argv, "c:d:s:ril:e:x:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'v': printf("fseventbridge %s (FsEventBridge)\n", FEB_VERSION); return 0;
            case 'h': print_usage(argv[0]); return 0;
            default:  break; // 忽略其他参数，留给第二次解析
        }
    }

    // 3. 加载配置 (优先级: 默认值 < 配置文件 < 命令行参数)
    if (!config_load(&config, config_file)) {
        print_usage(argv[0]);
        return 1;
    }

    // 第二次解析参数以支持命令行覆盖配置文件的值 (重置 optind)
    optind = 1;
    bool e_flag = false; // 标记是否处理过命令行 exclude-ext
    bool x_flag = false; // 标记是否处理过命令行 exclude-path

    while ((opt = getopt_long(argc, argv, "c:d:s:ril:e:x:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                strncpy(config.monitor_path, optarg, FEB_MAX_PATH - 1);
                config.monitor_path[FEB_MAX_PATH - 1] = '\0';
                break;
            case 's':
                strncpy(config.socket_path, optarg, FEB_MAX_PATH - 1);
                config.socket_path[FEB_MAX_PATH - 1] = '\0';
                break;
            case 'r':
                config.recursive = true;
                break;
            case 'i':
                config.use_io_uring = true;
                break;
            case OPT_NO_IO_URING:
                config.use_io_uring = false;
                break;
            case OPT_CHECK_CONFIG:
                check_config = true;
                break;
            case 'l':
                if (strcmp(optarg, "debug") == 0) config.log_level = FEB_LOG_DEBUG;
                else if (strcmp(optarg, "info") == 0) config.log_level = FEB_LOG_INFO;
                else if (strcmp(optarg, "warn") == 0) config.log_level = FEB_LOG_WARN;
                else if (strcmp(optarg, "error") == 0) config.log_level = FEB_LOG_ERROR;
                break;
            case 'e':
                if (!e_flag) {
                    free_string_array(&config.exclude_exts, &config.exclude_exts_count);
                    e_flag = true;
                }
                add_string_to_array(&config.exclude_exts, &config.exclude_exts_count, optarg);
                break;
            case 'x':
                if (!x_flag) {
                    free_string_array(&config.exclude_paths, &config.exclude_paths_count);
                    x_flag = true;
                }
                add_string_to_array(&config.exclude_paths, &config.exclude_paths_count, optarg);
                break;
        }
    }

    // 同步日志级别到全局变量
    SET_LOG_LEVEL(config.log_level);

    // 干跑：仅打印解析后的配置并退出，便于在没有 root 的环境下做自动化验证
    if (check_config) {
        print_resolved_config(&config);
        config_destroy(&config);
        return 0;
    }

    LOG_DEBUG("FsEventBridge is starting...");

    // 4. 注册信号
    signal(SIGINT, signal_handler);  // 按下 Ctrl+C 时调用 signal_handler
    signal(SIGTERM, signal_handler); // 接收到 kill 命令时调用 signal_handler

    // 5. 初始化各模块
    // 检查监控目录是否已设置
    if (strlen(config.monitor_path) == 0) {
        fprintf(stderr, "Error: Monitoring directory is not specified.\n");
        print_usage(argv[0]);
        return 1;
    }

    // 初始化 IPC (Unix Domain Socket)，建立通信渠道
    int ipc_fd = ipc_init(config.socket_path, &config);
    if (ipc_fd < 0) return 1;

    // 初始化监控器 (fanotify)，告诉内核要监控哪些文件
    int fan_fd = monitor_init(&config);
    if (fan_fd < 0) {
        // 如果监控器初始化失败，清理 IPC 资源
        ipc_cleanup(ipc_fd, config.socket_path);
        return 1;
    }

    // 6. 告知 systemd 服务已就绪
    sd_notify(0, "READY=1");
    LOG_INFO("FsEventBridge started successfully, monitoring directory: %s", config.monitor_path);

    // 7. 进入主循环 (将 running 标志传递给 monitor_loop)
    monitor_loop(fan_fd, ipc_fd, &config, &running);

    // 8. 优雅清理退出
    LOG_INFO("Stopping Service...");
    sd_notify(0, "STOPPING=1");
    
    // 关闭文件描述符，删除 Socket 文件
    monitor_cleanup(fan_fd);
    ipc_cleanup(ipc_fd, config.socket_path);
    config_destroy(&config);

    LOG_INFO("Service has exited safely.");
    return 0;
}
