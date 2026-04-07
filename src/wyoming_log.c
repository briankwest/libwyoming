/*
 * wyoming_log.c — Thread-safe logging with file + stderr output
 *
 * Messages go to both stderr (for journald) and an optional log file
 * (for persistent debug traces). File is opened in append mode and
 * flushed after each write.
 */

#include "wyoming_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

static wy_log_level_t g_level = WY_LOG_INFO;
static FILE          *g_file  = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str(wy_log_level_t level)
{
	switch (level) {
	case WY_LOG_ERROR: return "ERROR";
	case WY_LOG_WARN:  return "WARN ";
	case WY_LOG_INFO:  return "INFO ";
	case WY_LOG_DEBUG: return "DEBUG";
	default:           return "?????";
	}
}

void wy_log_init(wy_log_level_t level, const char *file_path)
{
	pthread_mutex_lock(&g_mutex);
	g_level = level;

	if (g_file && g_file != stderr) {
		fclose(g_file);
		g_file = NULL;
	}

	if (file_path && file_path[0]) {
		g_file = fopen(file_path, "a");
		if (!g_file)
			fprintf(stderr, "wyoming: cannot open log file: %s\n", file_path);
	}

	pthread_mutex_unlock(&g_mutex);
}

void wy_log_set_level(wy_log_level_t level)
{
	g_level = level;
}

wy_log_level_t wy_log_parse_level(const char *str)
{
	if (!str) return WY_LOG_INFO;
	if (strcmp(str, "error") == 0) return WY_LOG_ERROR;
	if (strcmp(str, "warn")  == 0) return WY_LOG_WARN;
	if (strcmp(str, "info")  == 0) return WY_LOG_INFO;
	if (strcmp(str, "debug") == 0) return WY_LOG_DEBUG;
	return WY_LOG_INFO;
}

void wy_log(wy_log_level_t level, const char *fmt, ...)
{
	if (level > g_level)
		return;

	/* Format timestamp */
	time_t now = time(NULL);
	struct tm tbuf;
	struct tm *t = localtime_r(&now, &tbuf);

	char ts[24];
	snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
	         t->tm_hour, t->tm_min, t->tm_sec);

	/* Format message */
	char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g_mutex);

	/* Always write to stderr (journald picks this up) */
	fprintf(stderr, "%s %s %s\n", ts, level_str(level), msg);

	/* Also write to file if configured */
	if (g_file) {
		/* Include date in file output */
		char datets[32];
		snprintf(datets, sizeof(datets), "%04d-%02d-%02d %02d:%02d:%02d",
		         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
		         t->tm_hour, t->tm_min, t->tm_sec);
		fprintf(g_file, "%s %s %s\n", datets, level_str(level), msg);
		fflush(g_file);
	}

	pthread_mutex_unlock(&g_mutex);
}

void wy_log_shutdown(void)
{
	pthread_mutex_lock(&g_mutex);
	if (g_file) {
		fclose(g_file);
		g_file = NULL;
	}
	pthread_mutex_unlock(&g_mutex);
}
