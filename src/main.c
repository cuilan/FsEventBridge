#include "fseventbridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
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

// 帮助信息
void print_usage(const char *prog_name) {
    printf("\n");
    printf("  ███████╗███████╗███████╗██╗   ██╗███████╗███╗   ██╗████████╗██████╗ ██████╗ ██╗██████╗  ██████╗ ███████╗\n");
    printf("  ██╔════╝██╔════╝██╔════╝██║   ██║██╔════╝████╗  ██║╚══██╔══╝██╔══██╗██╔══██╗██║██╔══██╗██╔════╝ ██╔════╝\n");
    printf("  █████╗  ███████╗█████╗  ██║   ██║█████╗  ██╔██╗ ██║   ██║   ██████╔╝██████╔╝██║██║  ██║██║  ███╗█████╗  \n");
    printf("  ██╔══╝  ╚════██║██╔══╝  ╚██╗ ██╔╝██╔══╝  ██║╚██╗██║   ██║   ██╔══██╗██╔══██╗██║██║  ██║██║   ██║██╔══╝  \n");
    printf("  ██║     ███████║███████╗ ╚████╔╝ ███████╗██║ ╚████║   ██║   ██████╔╝██║  ██║██║██████╔╝╚██████╔╝███████╗\n");
    printf("  ╚═╝     ╚══════╝╚══════╝  ╚═══╝  ╚══════╝╚═╝  ╚═══╝   ╚═╝   ╚═════╝ ╚═╝  ╚═╝╚═╝╚═════╝  ╚═════╝ ╚══════╝\n");
    printf("\n");
    printf("  High-Performance File System Event Bridge (Linux fanotify & io_uring)\n");
    printf("  Version: %s\n", FEB_VERSION);
    printf("  Repo: https://github.com/cuilan/FsEventBridge\n\n");

    printf("Usage:\n");
    printf("  %s [options]\n\n", prog_name);

    printf("Examples:\n");
    printf("  1. Monitor a directory recursively:\n");
    printf("     %s -d /data/logs -r\n\n", prog_name);
    printf("  2. Monitor with config file:\n");
    printf("     %s -c /etc/feb/config.toml\n\n", prog_name);
    printf("  3. Advanced usage (exclude extensions & paths, debug level):\n");
    printf("     %s -d /var/www -r -l debug -e .swp -e .tmp -x /var/www/cache\n\n", prog_name);

    printf("Options:\n");
    printf("  -c, --config PATH    Specify the TOML configuration file path\n");
    printf("  -d, --dir PATH       Specify the monitoring directory (override configuration)\n");
    printf("  -s, --socket PATH    Specify the Unix Socket path (default: /tmp/feb.sock)\n");
    printf("  -r, --recursive      Enable recursive monitoring (default: false)\n");
    printf("  -i, --io-uring       Enable io_uring optimization (default: true)\n");
    printf("  -l, --log-level      Specify the log level (debug, info, warn, error)\n");
    printf("  -e, --exclude-ext    Specify the exclude extension (multiple can be specified)\n");
    printf("  -x, --exclude-path   Specify the exclude path (multiple can be specified)\n");
    printf("  -v, --version        Display version information\n");
    printf("  -h, --help           Display this help information\n\n");
}

int main(int argc, char **argv) {
    feb_config_t config;
    char *config_file = NULL;
    int opt;

    // 1. 定义长选项
    static struct option long_options[] = {
        {"config",       required_argument, 0, 'c'},
        {"dir",          required_argument, 0, 'd'},
        {"socket",       required_argument, 0, 's'},
        {"recursive",    no_argument,       0, 'r'},
        {"io-uring",     no_argument,       0, 'i'},
        {"log-level",    required_argument, 0, 'l'},
        {"exclude-ext",  required_argument, 0, 'e'},
        {"exclude-path", required_argument, 0, 'x'},
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
            case 'v': printf("FsEventBridge Version: %s\n", FEB_VERSION); return 0;
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

    LOG_DEBUG("FsEventBridge is starting...");
    LOG_INFO("FsEventBridge is starting...");
    LOG_WARN("FsEventBridge is starting...");
    LOG_ERROR("FsEventBridge is starting...");

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
    int ipc_fd = ipc_init(config.socket_path);
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
    LOG_INFO("FsEventBridge Started Successfully, Monitoring Directory: %s", config.monitor_path);

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
