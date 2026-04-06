#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libwyoming/wyoming.h>
#include "cJSON.h"

/* From internal.h — we need iobuf for direct testing */
#define WYOMING_READBUF_SIZE 8192

typedef struct {
	int      fd;
	unsigned char buf[WYOMING_READBUF_SIZE];
	size_t   pos;
	size_t   len;
} wyoming_iobuf_t;

extern void            iobuf_init(wyoming_iobuf_t *io, int fd);
extern wyoming_error_t wyoming_event_read_iobuf(wyoming_iobuf_t *io,
                                                wyoming_event_t *event);
extern wyoming_error_t wyoming_event_write_fd(int fd,
                                              const wyoming_event_t *event);

extern void test_begin(const char *name);
extern void test_assert(int condition, const char *msg);
extern void test_end(void);

/* ── Test: basic event roundtrip with data ───────────────────── */

void test_event_roundtrip(void)
{
	test_begin("event_roundtrip");

	int fds[2];
	assert(pipe(fds) == 0);

	/* Write a synthesize event */
	cJSON *data = cJSON_CreateObject();
	cJSON_AddStringToObject(data, "text", "Hello world");

	wyoming_event_t out;
	wyoming_event_init(&out);
	out.type = strdup("synthesize");
	out.data = data;

	wyoming_error_t rc = wyoming_event_write_fd(fds[1], &out);
	test_assert(rc == WYOMING_OK, "write should succeed");

	/* Read it back */
	wyoming_iobuf_t io;
	iobuf_init(&io, fds[0]);

	wyoming_event_t in;
	wyoming_event_init(&in);
	rc = wyoming_event_read_iobuf(&io, &in);
	test_assert(rc == WYOMING_OK, "read should succeed");
	test_assert(strcmp(in.type, "synthesize") == 0, "type should match");
	test_assert(in.data != NULL, "data should be present");

	cJSON *text = cJSON_GetObjectItemCaseSensitive(in.data, "text");
	test_assert(cJSON_IsString(text), "text should be string");
	test_assert(strcmp(text->valuestring, "Hello world") == 0,
	            "text value should match");
	test_assert(in.payload == NULL, "no payload expected");

	wyoming_event_free(&in);
	wyoming_event_free(&out);
	close(fds[0]);
	close(fds[1]);

	test_end();
}

/* ── Test: event with no data ────────────────────────────────── */

void test_event_no_data(void)
{
	test_begin("event_no_data");

	int fds[2];
	assert(pipe(fds) == 0);

	wyoming_event_t out;
	wyoming_event_init(&out);
	out.type = strdup("audio-stop");

	wyoming_error_t rc = wyoming_event_write_fd(fds[1], &out);
	test_assert(rc == WYOMING_OK, "write should succeed");

	wyoming_iobuf_t io;
	iobuf_init(&io, fds[0]);

	wyoming_event_t in;
	wyoming_event_init(&in);
	rc = wyoming_event_read_iobuf(&io, &in);
	test_assert(rc == WYOMING_OK, "read should succeed");
	test_assert(strcmp(in.type, "audio-stop") == 0, "type should match");
	test_assert(in.data == NULL, "no data expected");
	test_assert(in.payload == NULL, "no payload expected");

	wyoming_event_free(&in);
	wyoming_event_free(&out);
	close(fds[0]);
	close(fds[1]);

	test_end();
}

/* ── Test: event with binary payload ─────────────────────────── */

void test_event_with_payload(void)
{
	test_begin("event_with_payload");

	int fds[2];
	assert(pipe(fds) == 0);

	int16_t samples[] = { 0, 100, -100, 32767, -32768 };
	size_t sample_bytes = sizeof(samples);

	cJSON *data = cJSON_CreateObject();
	cJSON_AddNumberToObject(data, "rate", 22050);
	cJSON_AddNumberToObject(data, "width", 2);
	cJSON_AddNumberToObject(data, "channels", 1);

	wyoming_event_t out;
	wyoming_event_init(&out);
	out.type = strdup("audio-chunk");
	out.data = data;
	out.payload = (uint8_t *)samples;
	out.payload_len = sample_bytes;

	wyoming_error_t rc = wyoming_event_write_fd(fds[1], &out);
	test_assert(rc == WYOMING_OK, "write should succeed");

	/* Don't let event_free touch our stack array */
	out.payload = NULL;
	out.payload_len = 0;

	wyoming_iobuf_t io;
	iobuf_init(&io, fds[0]);

	wyoming_event_t in;
	wyoming_event_init(&in);
	rc = wyoming_event_read_iobuf(&io, &in);
	test_assert(rc == WYOMING_OK, "read should succeed");
	test_assert(strcmp(in.type, "audio-chunk") == 0, "type should match");
	test_assert(in.payload_len == sample_bytes, "payload size should match");
	test_assert(memcmp(in.payload, samples, sample_bytes) == 0,
	            "payload bytes should match");

	cJSON *rate = cJSON_GetObjectItemCaseSensitive(in.data, "rate");
	test_assert(cJSON_IsNumber(rate) && (int)rate->valuedouble == 22050,
	            "rate should be 22050");

	wyoming_event_free(&in);
	wyoming_event_free(&out);
	close(fds[0]);
	close(fds[1]);

	test_end();
}

/* ── Test: multiple events on same pipe ──────────────────────── */

void test_event_multiple(void)
{
	test_begin("event_multiple");

	int fds[2];
	assert(pipe(fds) == 0);

	/* Write three events */
	const char *types[] = { "audio-start", "audio-chunk", "audio-stop" };
	for (int i = 0; i < 3; i++) {
		wyoming_event_t ev;
		wyoming_event_init(&ev);
		ev.type = strdup(types[i]);
		if (i == 0) {
			ev.data = cJSON_CreateObject();
			cJSON_AddNumberToObject(ev.data, "rate", 16000);
		}
		wyoming_event_write_fd(fds[1], &ev);
		wyoming_event_free(&ev);
	}

	/* Read all three back */
	wyoming_iobuf_t io;
	iobuf_init(&io, fds[0]);

	for (int i = 0; i < 3; i++) {
		wyoming_event_t ev;
		wyoming_event_init(&ev);
		wyoming_error_t rc = wyoming_event_read_iobuf(&io, &ev);
		test_assert(rc == WYOMING_OK, "read should succeed");
		test_assert(strcmp(ev.type, types[i]) == 0,
		            "type should match in sequence");
		wyoming_event_free(&ev);
	}

	close(fds[0]);
	close(fds[1]);

	test_end();
}
