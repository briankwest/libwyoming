#include "config.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

/* ── iobuf ───────────────────────────────────────────────────── */

void iobuf_init(wyoming_iobuf_t *io, int fd)
{
	io->fd  = fd;
	io->pos = 0;
	io->len = 0;
}

wyoming_error_t iobuf_read_line(wyoming_iobuf_t *io,
                                char **line_out, size_t *line_len)
{
	/* Dynamic line buffer for lines that exceed READBUF_SIZE */
	char  *line = NULL;
	size_t line_sz = 0;
	size_t line_used = 0;

	for (;;) {
		/* Scan buffered data for newline */
		for (size_t i = io->pos; i < io->len; i++) {
			if (io->buf[i] == '\n') {
				size_t chunk = i - io->pos;
				size_t total = line_used + chunk;

				char *result = malloc(total + 1);
				if (!result) {
					free(line);
					return WYOMING_ERR_NOMEM;
				}
				if (line_used > 0)
					memcpy(result, line, line_used);
				memcpy(result + line_used, io->buf + io->pos, chunk);
				result[total] = '\0';

				io->pos = i + 1; /* skip past \n */
				free(line);

				*line_out = result;
				if (line_len)
					*line_len = total;
				return WYOMING_OK;
			}
		}

		/* No newline found — save what we have and refill */
		size_t avail = io->len - io->pos;
		if (avail > 0) {
			size_t need = line_used + avail;
			if (need > line_sz) {
				line_sz = need * 2;
				if (line_sz < 256) line_sz = 256;
				char *tmp = realloc(line, line_sz);
				if (!tmp) { free(line); return WYOMING_ERR_NOMEM; }
				line = tmp;
			}
			memcpy(line + line_used, io->buf + io->pos, avail);
			line_used += avail;
		}

		/* Refill buffer */
		io->pos = 0;
		io->len = 0;
		for (;;) {
			ssize_t n = read(io->fd, io->buf, WYOMING_READBUF_SIZE);
			if (n > 0) {
				io->len = (size_t)n;
				break;
			}
			if (n == 0) {
				free(line);
				return WYOMING_ERR_EOF;
			}
			if (errno == EINTR)
				continue;
			free(line);
			return WYOMING_ERR_IO;
		}
	}
}

wyoming_error_t iobuf_read_exact(wyoming_iobuf_t *io,
                                 uint8_t *dest, size_t count)
{
	size_t done = 0;

	/* Drain buffered data first */
	size_t avail = io->len - io->pos;
	if (avail > 0) {
		size_t take = avail < count ? avail : count;
		memcpy(dest, io->buf + io->pos, take);
		io->pos += take;
		done += take;
	}

	/* Read remaining directly into dest */
	while (done < count) {
		ssize_t n = read(io->fd, dest + done, count - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n == 0) {
			return WYOMING_ERR_EOF;
		} else {
			if (errno == EINTR) continue;
			return WYOMING_ERR_IO;
		}
	}
	return WYOMING_OK;
}

/* ── Event init / free ───────────────────────────────────────── */

void wyoming_event_init(wyoming_event_t *event)
{
	memset(event, 0, sizeof(*event));
}

void wyoming_event_free(wyoming_event_t *event)
{
	if (!event) return;
	free(event->type);
	event->type = NULL;
	if (event->data) {
		cJSON_Delete(event->data);
		event->data = NULL;
	}
	free(event->payload);
	event->payload = NULL;
	event->payload_len = 0;
}

/* ── Event read ──────────────────────────────────────────────── */

wyoming_error_t wyoming_event_read_iobuf(wyoming_iobuf_t *io,
                                         wyoming_event_t *event)
{
	wyoming_error_t rc;
	char *line = NULL;

	wyoming_event_init(event);

	/* 1. Read header JSON line */
	rc = iobuf_read_line(io, &line, NULL);
	if (rc != WYOMING_OK)
		return rc;

	cJSON *header = cJSON_Parse(line);
	free(line);
	if (!header)
		return WYOMING_ERR_PARSE;

	/* 2. Extract type */
	cJSON *type_item = cJSON_GetObjectItemCaseSensitive(header, "type");
	if (!cJSON_IsString(type_item) || !type_item->valuestring) {
		cJSON_Delete(header);
		return WYOMING_ERR_PARSE;
	}
	event->type = strdup(type_item->valuestring);
	if (!event->type) {
		cJSON_Delete(header);
		return WYOMING_ERR_NOMEM;
	}

	/* 3. Extract data_length and payload_length */
	size_t data_length = 0;
	size_t payload_length = 0;

	cJSON *dl = cJSON_GetObjectItemCaseSensitive(header, "data_length");
	if (cJSON_IsNumber(dl) && dl->valuedouble > 0)
		data_length = (size_t)dl->valuedouble;

	cJSON *pl = cJSON_GetObjectItemCaseSensitive(header, "payload_length");
	if (cJSON_IsNumber(pl) && pl->valuedouble > 0)
		payload_length = (size_t)pl->valuedouble;

	cJSON_Delete(header);

	/* 4. Read data block */
	if (data_length > 0) {
		char *data_buf = malloc(data_length + 1);
		if (!data_buf) {
			wyoming_event_free(event);
			return WYOMING_ERR_NOMEM;
		}
		rc = iobuf_read_exact(io, (uint8_t *)data_buf, data_length);
		if (rc != WYOMING_OK) {
			free(data_buf);
			wyoming_event_free(event);
			return rc;
		}
		data_buf[data_length] = '\0';
		event->data = cJSON_Parse(data_buf);
		free(data_buf);
		if (!event->data) {
			wyoming_event_free(event);
			return WYOMING_ERR_PARSE;
		}
	}

	/* 5. Read binary payload */
	if (payload_length > 0) {
		event->payload = malloc(payload_length);
		if (!event->payload) {
			wyoming_event_free(event);
			return WYOMING_ERR_NOMEM;
		}
		rc = iobuf_read_exact(io, event->payload, payload_length);
		if (rc != WYOMING_OK) {
			wyoming_event_free(event);
			return rc;
		}
		event->payload_len = payload_length;
	}

	return WYOMING_OK;
}

/* ── Event write ─────────────────────────────────────────────── */

static wyoming_error_t write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0) {
			if (errno == EINTR) continue;
			return WYOMING_ERR_IO;
		}
	}
	return WYOMING_OK;
}

wyoming_error_t wyoming_event_write_fd(int fd,
                                       const wyoming_event_t *event)
{
	if (!event || !event->type)
		return WYOMING_ERR_INVAL;

	/* Serialize data to JSON string if present */
	char *data_str = NULL;
	size_t data_len = 0;
	if (event->data) {
		data_str = cJSON_PrintUnformatted(event->data);
		if (!data_str)
			return WYOMING_ERR_NOMEM;
		data_len = strlen(data_str);
	}

	/* Build header JSON */
	cJSON *header = cJSON_CreateObject();
	if (!header) {
		free(data_str);
		return WYOMING_ERR_NOMEM;
	}
	cJSON_AddStringToObject(header, "type", event->type);
	cJSON_AddStringToObject(header, "version", LIBWYOMING_VERSION_STRING);
	if (data_len > 0)
		cJSON_AddNumberToObject(header, "data_length", (double)data_len);
	if (event->payload && event->payload_len > 0)
		cJSON_AddNumberToObject(header, "payload_length",
		                        (double)event->payload_len);

	char *header_str = cJSON_PrintUnformatted(header);
	cJSON_Delete(header);
	if (!header_str) {
		free(data_str);
		return WYOMING_ERR_NOMEM;
	}

	/* Write segments sequentially */
	wyoming_error_t rc;
	size_t header_len = strlen(header_str);

	rc = write_all(fd, header_str, header_len);
	if (rc == WYOMING_OK)
		rc = write_all(fd, "\n", 1);
	if (rc == WYOMING_OK && data_str && data_len > 0)
		rc = write_all(fd, data_str, data_len);
	if (rc == WYOMING_OK && event->payload && event->payload_len > 0)
		rc = write_all(fd, event->payload, event->payload_len);

	free(header_str);
	free(data_str);
	return rc;
}
