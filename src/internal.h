#ifndef LIBWYOMING_INTERNAL_H
#define LIBWYOMING_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <libwyoming/wyoming.h>
#include "cJSON.h"

/* ── Buffered reader ─────────────────────────────────────────── */
#define WYOMING_READBUF_SIZE 8192

typedef struct {
	int      fd;
	uint8_t  buf[WYOMING_READBUF_SIZE];
	size_t   pos;   /* current read position */
	size_t   len;   /* valid bytes in buf */
} wyoming_iobuf_t;

void            iobuf_init(wyoming_iobuf_t *io, int fd);
wyoming_error_t iobuf_read_line(wyoming_iobuf_t *io,
                                char **line_out, size_t *line_len);
wyoming_error_t iobuf_read_exact(wyoming_iobuf_t *io,
                                 uint8_t *dest, size_t count);

/* ── Internal event I/O (takes iobuf) ────────────────────────── */
wyoming_error_t wyoming_event_read_iobuf(wyoming_iobuf_t *io,
                                         wyoming_event_t *event);
wyoming_error_t wyoming_event_write_fd(int fd,
                                       const wyoming_event_t *event);

/* ── Client connection (internal) ────────────────────────────── */
struct wyoming_conn {
	int              fd;
	wyoming_iobuf_t  iobuf;
};

/* ── Server voice storage ────────────────────────────────────── */
#define WYOMING_MAX_VOICES    64
#define WYOMING_MAX_SPEAKERS  32
#define WYOMING_MAX_LANGS     8

typedef struct {
	char *name;
	char *languages[WYOMING_MAX_LANGS];
	int   language_count;
	char *description;
	char *speakers[WYOMING_MAX_SPEAKERS];
	int   speaker_count;
} server_voice_t;

/* ── Server ASR model storage ───────────────────────────────── */
#define WYOMING_MAX_MODELS    16

typedef struct {
	char *name;
	char *languages[WYOMING_MAX_LANGS];
	int   language_count;
	char *description;
} server_model_t;

/* ── Helper ──────────────────────────────────────────────────── */
static inline int wy_min(int a, int b) { return a < b ? a : b; }

#endif /* LIBWYOMING_INTERNAL_H */
