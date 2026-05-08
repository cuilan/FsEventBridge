#include "fseventbridge.h"
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

/**
 * 将字符串转义为符合 JSON 规范的字符串
 *
 * @param dest   目标缓冲区
 * @param size   目标缓冲区大小
 * @param src    源字符串
 * @return       成功返回转义后的长度，失败（空间不足）返回 -1
 */
ssize_t escape_json_string(char *dest, size_t size, const char *src) {
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0'; s++) {
        // 预留空间检查：最坏情况一个字符变 6 个 (\uXXXX) + 1 个结束符
        if (d + 7 > size) return -1;

        unsigned char c = (unsigned char)src[s];
        switch (c) {
            case '\"': dest[d++] = '\\'; dest[d++] = '\"'; break;
            case '\\': dest[d++] = '\\'; dest[d++] = '\\'; break;
            case '\b': dest[d++] = '\\'; dest[d++] = 'b';  break;
            case '\f': dest[d++] = '\\'; dest[d++] = 'f';  break;
            case '\n': dest[d++] = '\\'; dest[d++] = 'n';  break;
            case '\r': dest[d++] = '\\'; dest[d++] = 'r';  break;
            case '\t': dest[d++] = '\\'; dest[d++] = 't';  break;
            default:
                if (c < 32) {
                    // 控制字符转义为 \u00xx
                    d += snprintf(&dest[d], 7, "\\u%04x", c);
                } else {
                    dest[d++] = c;
                }
        }
    }
    dest[d] = '\0';
    return (ssize_t)d;
}

// --- JSON Writer 实现 ---

void json_init(json_writer_t *w, char *buf, size_t size) {
    w->buf = buf;
    w->size = size;
    w->offset = 0;
    w->error = false;
    w->first_field = true;
    if (size > 0) buf[0] = '\0';
}

static void json_append(json_writer_t *w, const char *fmt, ...) {
    if (w->error) return;
    
    va_list args;
    va_start(args, fmt);
    
    // 计算剩余空间
    size_t remain = w->size - w->offset;
    int len = vsnprintf(w->buf + w->offset, remain, fmt, args);
    
    va_end(args);
    
    if (len < 0 || (size_t)len >= remain) {
        w->error = true;
    } else {
        w->offset += len;
    }
}

void json_start_object(json_writer_t *w) {
    json_append(w, "{");
    w->first_field = true;
}

void json_end_object(json_writer_t *w) {
    json_append(w, "}");
}

static void json_comma(json_writer_t *w) {
    if (!w->first_field) {
        json_append(w, ",");
    }
    w->first_field = false;
}

void json_key_string(json_writer_t *w, const char *key, const char *val) {
    if (w->error) return;
    json_comma(w);
    
    json_append(w, "\"%s\":\"", key);
    
    // 手动处理值转义，避免使用额外的大缓冲区
    if (!w->error) {
        size_t remain = w->size - w->offset;
        // 使用 -1 以保留末尾的 " 和 }
        ssize_t len = escape_json_string(w->buf + w->offset, remain, val);
        if (len < 0) {
            w->error = true;
        } else {
            w->offset += len;
        }
    }
    
    json_append(w, "\"");
}

void json_key_uint(json_writer_t *w, const char *key, uint64_t val) {
    json_comma(w);
    json_append(w, "\"%s\":%lu", key, val);
}

void json_key_int(json_writer_t *w, const char *key, int64_t val) {
    json_comma(w);
    json_append(w, "\"%s\":%ld", key, val);
}

// 事件类型 -> 可读字符串（与 NDJSON 字段 event 保持一致）
const char *feb_event_name(feb_event_type_t t) {
    switch (t) {
        case FEB_EVENT_CLOSE_WRITE: return "CLOSE_WRITE";
        case FEB_EVENT_MOVED_TO:    return "MOVED_TO";
        case FEB_EVENT_MOVED_FROM:  return "MOVED_FROM";
        case FEB_EVENT_CREATE:      return "CREATE";
        case FEB_EVENT_DELETE:      return "DELETE";
        case FEB_EVENT_MODIFY:      return "MODIFY";
        case FEB_EVENT_UNKNOWN:
        default:                    return "UNKNOWN";
    }
}

