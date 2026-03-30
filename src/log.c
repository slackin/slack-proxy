/*
 * log.c — Simple timestamped, levelled logger writing to stderr.
 *
 * Output format:
 *   [YYYY-MM-DD HH:MM:SS] [LEVEL] message\n
 *
 * Thread safety: NOT thread-safe, but urt-proxy is single-threaded so
 * this is fine.  If multi-threading were ever added, g_min_level and
 * the fprintf calls would need synchronisation.
 */

#define _POSIX_C_SOURCE 200809L

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* Minimum severity level — messages below this are silently dropped. */
static log_level_t g_min_level = LOG_INFO;

/* Human-readable names for each log level, indexed by log_level_t. */
static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

/*
 * log_init — Set the global minimum log level.
 *
 * Should be called once from main() before any logging occurs.
 */
void log_init(log_level_t min_level)
{
    g_min_level = min_level;
}

/*
 * log_msg — Emit a formatted log message to stderr.
 *
 * Prepends a local-time timestamp and the level tag, then prints the
 * caller's printf-style message.  Messages below g_min_level are
 * dropped immediately.
 */
void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level < g_min_level)
        return;

    /* Build a human-readable timestamp (local time) */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    /* Print: [timestamp] [LEVEL] <message>\n */
    fprintf(stderr, "[%s] [%-5s] ", timebuf, level_str[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
