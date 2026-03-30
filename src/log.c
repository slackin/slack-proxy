#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static log_level_t g_min_level = LOG_INFO;

static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

void log_init(log_level_t min_level)
{
    g_min_level = min_level;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level < g_min_level)
        return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[%s] [%-5s] ", timebuf, level_str[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
