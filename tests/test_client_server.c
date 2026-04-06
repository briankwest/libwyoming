#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <libwyoming/wyoming.h>

extern void test_begin(const char *name);
extern void test_assert(int condition, const char *msg);
extern void test_end(void);

/* ── Mock TTS callback ───────────────────────────────────────── */

static wyoming_error_t mock_tts(const char *text,
                                const char *voice,
                                const char *speaker,
                                int16_t **pcm_out,
                                size_t *samples_out,
                                wyoming_audio_format_t *format_out,
                                void *userdata)
{
	(void)voice;
	(void)speaker;
	(void)userdata;

	/* Generate a simple tone based on text length */
	size_t len = strlen(text);
	size_t n_samples = len * 100; /* 100 samples per character */

	int16_t *pcm = malloc(n_samples * sizeof(int16_t));
	if (!pcm) return WYOMING_ERR_NOMEM;

	for (size_t i = 0; i < n_samples; i++)
		pcm[i] = (int16_t)(i & 0x7FFF);

	*pcm_out = pcm;
	*samples_out = n_samples;
	format_out->rate = 22050;
	format_out->width = 2;
	format_out->channels = 1;

	return WYOMING_OK;
}

/* ── Server thread ───────────────────────────────────────────── */

struct server_ctx {
	wyoming_server_t *srv;
	int               ready; /* set to 1 after server starts listening */
};

static void *server_thread(void *arg)
{
	struct server_ctx *ctx = arg;
	ctx->ready = 1;
	wyoming_server_run(ctx->srv);
	return NULL;
}

/* ── Test: describe ──────────────────────────────────────────── */

void test_client_server_describe(void)
{
	test_begin("client_server_describe");

	wyoming_server_t *srv = wyoming_server_create("127.0.0.1", 0);
	test_assert(srv != NULL, "server create should succeed");
	if (!srv) { test_end(); return; }

	uint16_t port = wyoming_server_port(srv);
	test_assert(port > 0, "port should be assigned");

	wyoming_server_set_tts(srv, mock_tts, NULL, "test-tts", "1.0.0");
	const char *langs[] = { "en_US", NULL };
	wyoming_server_add_voice(srv, "test-voice", langs, "A test voice");

	struct server_ctx ctx = { .srv = srv, .ready = 0 };
	pthread_t tid;
	pthread_create(&tid, NULL, server_thread, &ctx);

	/* Wait for server to be ready */
	while (!ctx.ready)
		usleep(1000);
	usleep(50000); /* extra settle time */

	wyoming_conn_t *conn = wyoming_connect("127.0.0.1", port);
	test_assert(conn != NULL, "client connect should succeed");

	if (conn) {
		wyoming_info_t info;
		wyoming_error_t rc = wyoming_describe(conn, &info);
		test_assert(rc == WYOMING_OK, "describe should succeed");

		if (rc == WYOMING_OK) {
			test_assert(info.tts_count == 1, "should have 1 tts program");
			test_assert(strcmp(info.tts[0].name, "test-tts") == 0,
			            "tts name should match");
			test_assert(info.tts[0].voice_count == 1,
			            "should have 1 voice");
			test_assert(strcmp(info.tts[0].voices[0].name,
			                   "test-voice") == 0,
			            "voice name should match");
			wyoming_info_free(&info);
		}

		wyoming_close(conn);
	}

	wyoming_server_stop(srv);
	pthread_join(tid, NULL);
	wyoming_server_destroy(srv);

	test_end();
}

/* ── Test: synthesize ────────────────────────────────────────── */

void test_client_server_synthesize(void)
{
	test_begin("client_server_synthesize");

	wyoming_server_t *srv = wyoming_server_create("127.0.0.1", 0);
	test_assert(srv != NULL, "server create should succeed");
	if (!srv) { test_end(); return; }

	uint16_t port = wyoming_server_port(srv);

	wyoming_server_set_tts(srv, mock_tts, NULL, "test-tts", "1.0.0");
	const char *langs[] = { "en_US", NULL };
	wyoming_server_add_voice(srv, "test-voice", langs, NULL);

	struct server_ctx ctx = { .srv = srv, .ready = 0 };
	pthread_t tid;
	pthread_create(&tid, NULL, server_thread, &ctx);

	while (!ctx.ready)
		usleep(1000);
	usleep(50000);

	wyoming_conn_t *conn = wyoming_connect("127.0.0.1", port);
	test_assert(conn != NULL, "client connect should succeed");

	if (conn) {
		int16_t *pcm = NULL;
		size_t samples = 0;
		wyoming_audio_format_t fmt = { 0 };

		wyoming_error_t rc = wyoming_synthesize_pcm(
			conn, "Hello", NULL, &pcm, &samples, &fmt);
		test_assert(rc == WYOMING_OK, "synthesize_pcm should succeed");

		if (rc == WYOMING_OK) {
			/* "Hello" = 5 chars * 100 samples = 500 samples */
			test_assert(samples == 500,
			            "should have 500 samples");
			test_assert(fmt.rate == 22050, "rate should be 22050");
			test_assert(fmt.width == 2, "width should be 2");
			test_assert(fmt.channels == 1, "channels should be 1");
			test_assert(pcm != NULL, "pcm should not be NULL");
		}

		free(pcm);
		wyoming_close(conn);
	}

	wyoming_server_stop(srv);
	pthread_join(tid, NULL);
	wyoming_server_destroy(srv);

	test_end();
}
