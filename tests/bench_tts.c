/*
 * bench_tts.c — TTS latency benchmark
 *
 * Measures total synthesis time (connect → PCM received).
 * Runs multiple iterations to show cold vs warm performance.
 *
 * Usage: bench_tts [host] [port] [text]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libwyoming/wyoming.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 10200;
	const char *text = argc > 3 ? argv[3] : "The quick brown fox jumps over the lazy dog.";

	printf("TTS Latency Benchmark\n");
	printf("Server: %s:%d\n", host, port);
	printf("Text: \"%s\" (%zu chars)\n\n", text, strlen(text));

	for (int run = 0; run < 5; run++) {
		double t_start = now_ms();

		wyoming_conn_t *conn = wyoming_connect(host, port);
		if (!conn) {
			fprintf(stderr, "Connect failed\n");
			return 1;
		}

		double t_connected = now_ms();

		int16_t *pcm = NULL;
		size_t samples = 0;
		wyoming_audio_format_t fmt = {0};

		wyoming_error_t rc = wyoming_synthesize_pcm(conn, text, NULL,
		                                             &pcm, &samples, &fmt);
		double t_done = now_ms();

		wyoming_close(conn);

		if (rc == WYOMING_OK && pcm) {
			double audio_dur = fmt.rate > 0 ? (double)samples / fmt.rate : 0;
			printf("Run %d: connect=%.0fms  synth=%.0fms  total=%.0fms  "
			       "audio=%.1fs (%zu samples @ %dHz)  RTF=%.2f\n",
			       run + 1,
			       t_connected - t_start,
			       t_done - t_connected,
			       t_done - t_start,
			       audio_dur, samples, fmt.rate,
			       (t_done - t_start) / 1000.0 / (audio_dur > 0 ? audio_dur : 1));
			free(pcm);
		} else {
			printf("Run %d: FAILED (%d)\n", run + 1, rc);
		}
	}

	return 0;
}
