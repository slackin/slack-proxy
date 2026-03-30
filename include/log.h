#ifndef URT_LOG_H
#define URT_LOG_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

void log_init(log_level_t min_level);
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif
