#include "fseventbridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>

// 全局运行标志，sig_atomic_t 确保信号处理的原子性
static volatile sig_atomic_t running = 1;

// 信号处理函数：捕捉 Ctrl+C 或 systemctl stop
static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

// 帮助信息
static void print_usage(const char *prog_name) {
    printf("用法: %s [选项]\n", prog_name);
    printf("选项:\n");
    printf("  -c, --config PATH    指定 TOML 配置文件路径\n");
    printf("  -d, --dir PATH       指定监控目录 (覆盖配置)\n");
    printf("  -s, --socket PATH    指定 Unix Socket 路径 (覆盖配置)\n");
    printf("  -v, --version        显示版本信息\n");
    printf("  -h, --help           显示此帮助信息\n");
}

int main(int argc, char **argv) {
    feb_config_t config;
    char *config_file = NULL;
    int opt;

    // 1. 定义长选项
    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"dir",     required_argument, 0, 'd'},
        {"socket",  required_argument, 0, 's'},
        {"version", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    // 2. 解析命令行参数
    while ((opt = getopt_long(argc, argv, "c:d:s:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'v': printf("FsEventBridge 版本: %s\n", FEB_VERSION); return 0;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    // 3. 加载配置 (优先级: 默认值 < 配置文件 < 命令行参数)
    if (!config_load(&config, config_file)) {
        return 1;
    }

    // 再次解析参数以支持命令行覆盖配置文件的值 (重置 optind)
    optind = 1;
    while ((opt = getopt_long(argc, argv, "c:d:s:vh", long_options, NULL)) != -1) {
        if (opt == 'd') strncpy(config.monitor_path, optarg, FEB_MAX_PATH);
        if (opt == 's') strncpy(config.socket_path, optarg, FEB_MAX_PATH);
    }

    // 4. 注册信号
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 5. 初始化各模块
    int ipc_fd = ipc_init(config.socket_path);
    if (ipc_fd < 0) return 1;

    int fan_fd = monitor_init(&config);
    if (fan_fd < 0) {
        ipc_cleanup(ipc_fd, config.socket_path);
        return 1;
    }

    // 6. 告知 systemd 服务已就绪
    sd_notify(0, "READY=1");
    printf("[MAIN] FsEventBridge 启动成功，监控目录: %s\n", config.monitor_path);

    // 7. 进入主循环 (将 running 标志传递给 monitor_loop)
    // 修改后的 monitor_loop 内部应检查 running 标志
    monitor_loop(fan_fd, ipc_fd, &config, &running);

    // 8. 优雅清理退出
    printf("[MAIN] 正在停止服务...\n");
    sd_notify(0, "STOPPING=1");
    
    monitor_cleanup(fan_fd);
    ipc_cleanup(ipc_fd, config.socket_path);
    config_destroy(&config);

    printf("[MAIN] 服务已安全退出。\n");
    return 0;
}