#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <liburing.h>

// 限制与定义
#define FEB_MAX_PATH PATH_MAX

#ifndef FEB_VERSION
#define FEB_VERSION "unknown"
#endif

// 日志级别
typedef enum {
    FEB_LOG_DEBUG = 0,
    FEB_LOG_INFO,
    FEB_LOG_WARN,
    FEB_LOG_ERROR
} feb_log_level_t;

// 声明全局日志级别变量（在 main.c 中定义）
extern feb_log_level_t g_log_level;

// 日志核心宏
#define LOG_BASE(level, level_str, color, fmt, ...) \
    do { \
        if (level >= g_log_level) { \
            time_t now = time(NULL); \
            struct tm *tm_info = localtime(&now); \
            char time_buf[26]; \
            strftime(time_buf, 26, "%Y-%m-%d %H:%M:%S", tm_info); \
            fprintf(stderr, color "[%s] [%s] " fmt "\x1b[0m\n", \
                    time_buf, level_str, ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_DEBUG(fmt, ...) LOG_BASE(FEB_LOG_DEBUG, "DEBUG", "\x1b[36m", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_BASE(FEB_LOG_INFO,  "INFO ", "\x1b[32m", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_BASE(FEB_LOG_WARN,  "WARN ", "\x1b[33m", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_BASE(FEB_LOG_ERROR, "ERROR", "\x1b[31m", fmt ": %s", ##__VA_ARGS__, strerror(errno))

// 辅助宏：设置全局日志级别
#define SET_LOG_LEVEL(level) g_log_level = level

// 辅助宏：获取全局日志级别
#define GET_LOG_LEVEL() g_log_level

// 核心配置结构体
typedef struct {
    char monitor_path[FEB_MAX_PATH];   // 监控的目标路径
    char socket_path[FEB_MAX_PATH];    // Unix Domain Socket 路径
    bool recursive;                    // 是否递归监控 (fanotify 默认为挂载点监控)
    bool use_io_uring;                 // 是否启用 io_uring 优化
    feb_log_level_t log_level;         // 日志级别
    
    // 监控的事件类型
    uint32_t event_mask;              // fanotify 事件掩码
    
    // 过滤规则
    char **exclude_exts;               // 排除的扩展名列表
    int exclude_exts_count;
    char **exclude_paths;              // 排除的路径列表
    int exclude_paths_count;
} feb_config_t;

// 文件事件类型枚举（与 NDJSON 中 type 整数一致；首个值为未知）
typedef enum {
    FEB_EVENT_UNKNOWN = 0,
    FEB_EVENT_CLOSE_WRITE,
    FEB_EVENT_MOVED_TO,
    FEB_EVENT_MOVED_FROM,
    FEB_EVENT_CREATE,
    FEB_EVENT_DELETE,
    FEB_EVENT_MODIFY
} feb_event_type_t;

// 文件事件模型
typedef struct {
    char path[FEB_MAX_PATH];           // 发生变动的文件路径，非指针，牺牲栈空间，减少了内存碎片和 malloc 失败的风险
    uint64_t size;                     // 文件当前大小（字节）
    // ts：网关处理该 fanotify 事件时的墙钟时间（CLOCK_REALTIME，Unix 纪元秒）
    int64_t ts;
    // mtime：fstat 得到的文件内容最后修改时间（st_mtim 秒）；fstat 失败时为 -1
    int64_t mtime;
    uint32_t mask;                     // 内核原始事件掩码 (fanotify mask)
    feb_event_type_t event_type;       // 事件类型枚举
} feb_event_t;

// io_uring 上下文
typedef struct {
    int ring_fd;
    struct io_uring ring;
} feb_io_uring_t;

// 模块接口声明
void print_usage(const char *prog_name);
bool config_load(feb_config_t *config, const char *path);
void config_destroy(feb_config_t *config);

// io_uring 初始化/销毁
int io_uring_init(feb_io_uring_t *ctx);
void io_uring_cleanup(feb_io_uring_t *ctx);

// 工具函数
ssize_t escape_json_string(char *dest, size_t size, const char *src);

// 简单的 JSON 写入器
typedef struct {
    char *buf;
    size_t size;
    size_t offset;
    bool error;
    bool first_field;
} json_writer_t;

void json_init(json_writer_t *w, char *buf, size_t size);
void json_start_object(json_writer_t *w);
void json_end_object(json_writer_t *w);
void json_key_string(json_writer_t *w, const char *key, const char *val);
void json_key_uint(json_writer_t *w, const char *key, uint64_t val);
void json_key_int(json_writer_t *w, const char *key, int64_t val);

// 事件类型 -> 可读名称（与 NDJSON 中 event 字段一致）
const char *feb_event_name(feb_event_type_t t);

int monitor_init(const feb_config_t *config);
void monitor_loop(int fan_fd, int ipc_fd, const feb_config_t *config, volatile sig_atomic_t *running);
void monitor_cleanup(int fan_fd);

int ipc_init(const char *socket_path);
void ipc_broadcast(int server_fd, const feb_event_t *event);
void ipc_cleanup(int server_fd, const char *socket_path);