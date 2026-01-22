#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>

// 限制与定义
#define FEB_MAX_PATH PATH_MAX
#define FEB_VERSION "1.0.0"

// 日志级别
typedef enum {
    FEB_LOG_DEBUG = 0,
    FEB_LOG_INFO,
    FEB_LOG_WARN,
    FEB_LOG_ERROR
} feb_log_level_t;

// 核心配置结构体
typedef struct {
    char monitor_path[FEB_MAX_PATH];   // 监控的目标路径
    char socket_path[FEB_MAX_PATH];    // Unix Domain Socket 路径
    bool recursive;                    // 是否递归监控 (fanotify 默认为挂载点监控)
    bool use_io_uring;                 // 是否启用 io_uring 优化
    feb_log_level_t log_level;         // 日志级别
    
    // 过滤规则
    char **exclude_exts;               // 排除的扩展名列表
    int exclude_exts_count;
} feb_config_t;

// 文件事件模型
typedef struct {
    char path[FEB_MAX_PATH];           // 发生变动的文件路径，非指针，牺牲栈空间，减少了内存碎片和 malloc 失败的风险
    uint64_t size;                     // 文件当前大小
    int64_t timestamp;                 // 事件发生的时间戳 (Unix Epoch)
    uint32_t mask;                     // 内核原始事件掩码 (fanotify mask)
} feb_event_t;

// 模块接口声明
bool config_load(feb_config_t *config, const char *path);
void config_destroy(feb_config_t *config);

int monitor_init(const feb_config_t *config);
void monitor_loop(int fan_fd, const feb_config_t *config);

int ipc_init(const char *socket_path);
void ipc_broadcast(int server_fd, const feb_event_t *event);