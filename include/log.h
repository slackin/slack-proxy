/*
 * log.h — Timestamped, levelled logging to stderr.
 *
 * All output goes to stderr in the format:
 *   [YYYY-MM-DD HH:MM:SS] [LEVEL] message
 *
 * Use the convenience macros (log_debug, log_info, log_warn, log_error)
 * rather than calling log_msg() directly.
 */

#ifndef URT_LOG_H
#define URT_LOG_H

/*
 * log_level_t — Severity levels, ordered from most to least verbose.
 *
 * Messages below the minimum level set by log_init() are silently dropped.
 */
typedef enum {
    LOG_DEBUG,  /* Verbose diagnostics (enabled with -d / --debug) */
    LOG_INFO,   /* Normal operational events (default threshold)   */
    LOG_WARN,   /* Non-fatal problems (rate limits, capacity hits) */
    LOG_ERROR   /* Fatal or unrecoverable errors                   */
} log_level_t;

/*
 * log_init — Set the minimum log level.
 *
 * Call once at startup before any log_msg() / macro usage.
 *
 * @param min_level  Messages below this level are suppressed.
 */
void log_init(log_level_t min_level);

/*
 * log_msg — Emit a log message with a timestamp and level tag.
 *
 * Prefer the convenience macros below instead of calling this directly.
 *
 * @param level  Severity of the message.
 * @param fmt    printf-style format string.
 * @param ...    Format arguments.
 */
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros — automatically fill in the level argument. */
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif
