/*
 * wyoming_log.h — Logging for Wyoming server and library
 */

#ifndef WYOMING_LOG_H
#define WYOMING_LOG_H

typedef enum {
	WY_LOG_ERROR = 0,
	WY_LOG_WARN  = 1,
	WY_LOG_INFO  = 2,
	WY_LOG_DEBUG = 3,
} wy_log_level_t;

/* Initialize logging. file_path=NULL means stderr only.
 * Both stderr and file receive messages at or below the configured level. */
void wy_log_init(wy_log_level_t level, const char *file_path);

/* Change level at runtime (e.g. after config reload) */
void wy_log_set_level(wy_log_level_t level);

/* Parse level string: "error", "warn", "info", "debug" */
wy_log_level_t wy_log_parse_level(const char *str);

/* Log a message. Use the macros below. */
void wy_log(wy_log_level_t level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Shut down logging, flush and close file */
void wy_log_shutdown(void);

/* Convenience macros */
#define WY_LOGE(...) wy_log(WY_LOG_ERROR, __VA_ARGS__)
#define WY_LOGW(...) wy_log(WY_LOG_WARN,  __VA_ARGS__)
#define WY_LOGI(...) wy_log(WY_LOG_INFO,  __VA_ARGS__)
#define WY_LOGD(...) wy_log(WY_LOG_DEBUG, __VA_ARGS__)

#endif /* WYOMING_LOG_H */
