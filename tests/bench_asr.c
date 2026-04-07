/*
 * bench_asr.c — ASR latency benchmark (time to first recognition)
 *
 * Two modes:
 *   batch:     Sends all audio at once, measures time to transcript
 *   streaming: Sends audio chunks in real-time, measures time from
 *              last chunk to final transcript (effective latency)
 *
 * Usage: bench_asr [host] [port] [wav_file] [--streaming]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libwyoming/wyoming.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int read_wav(const char *path, int16_t **out, size_t *samples, int *rate)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	char hdr[44];
	if (fread(hdr, 1, 44, f) != 44) { fclose(f); return -1; }
	*rate = *(int32_t *)(hdr + 24);
	int32_t data_size = *(int32_t *)(hdr + 40);
	*samples = data_size / 2;
	*out = malloc(data_size);
	if (!*out) { fclose(f); return -1; }
	fread(*out, 1, data_size, f);
	fclose(f);
	return 0;
}

static int run_batch(const char *host, uint16_t port,
                     int16_t *pcm, size_t samples, int rate)
{
	printf("\n--- Batch mode ---\n");

	for (int run = 0; run < 3; run++) {
		wyoming_conn_t *conn = wyoming_connect(host, port);
		if (!conn) { fprintf(stderr, "Connect failed\n"); return 1; }

		wyoming_audio_format_t fmt = { .rate = rate, .width = 2, .channels = 1 };

		double t_start = now_ms();

		char *text = NULL;
		wyoming_error_t rc = wyoming_transcribe_pcm(conn, pcm, samples,
		                                             &fmt, "en", &text);
		double t_done = now_ms();
		wyoming_close(conn);

		double audio_dur = (double)samples / rate;
		double latency = t_done - t_start;

		if (rc == WYOMING_OK && text) {
			printf("Run %d: latency=%.0fms  audio=%.1fs  RTF=%.2f  text=\"%s\"\n",
			       run + 1, latency, audio_dur, latency / 1000.0 / audio_dur, text);
			free(text);
		} else {
			printf("Run %d: FAILED (%d)\n", run + 1, rc);
		}
	}
	return 0;
}

static int run_streaming(const char *host, uint16_t port,
                         int16_t *pcm, size_t samples, int rate)
{
	printf("\n--- Streaming mode ---\n");

	/* Simulate real-time: send chunks at the audio's natural rate */
	size_t chunk_samples = (size_t)rate / 50;  /* 20ms chunks */
	double chunk_duration_ms = (double)chunk_samples / rate * 1000.0;

	for (int run = 0; run < 3; run++) {
		wyoming_conn_t *conn = wyoming_connect(host, port);
		if (!conn) { fprintf(stderr, "Connect failed\n"); return 1; }

		wyoming_audio_format_t fmt = { .rate = rate, .width = 2, .channels = 1 };

		double t_start = now_ms();

		wyoming_error_t rc = wyoming_transcribe_start(conn, &fmt, "en");
		if (rc != WYOMING_OK) {
			fprintf(stderr, "transcribe_start failed: %d\n", rc);
			wyoming_close(conn);
			return 1;
		}

		/* Feed chunks in real-time */
		size_t offset = 0;
		int chunks_sent = 0;
		while (offset < samples) {
			size_t n = samples - offset;
			if (n > chunk_samples) n = chunk_samples;

			rc = wyoming_transcribe_chunk(conn, pcm + offset, n, &fmt);
			if (rc != WYOMING_OK) {
				fprintf(stderr, "chunk %d failed: %d\n", chunks_sent, rc);
				break;
			}
			offset += n;
			chunks_sent++;

			/* Sleep to simulate real-time playback */
			usleep((unsigned)(chunk_duration_ms * 800));  /* 80% of real-time */
		}

		double t_audio_done = now_ms();

		/* Stop and get final transcript */
		char *text = NULL;
		rc = wyoming_transcribe_stop(conn, &text);

		double t_transcript = now_ms();
		wyoming_close(conn);

		double audio_dur = (double)samples / rate;
		double total = t_transcript - t_start;
		double post_audio_latency = t_transcript - t_audio_done;

		if (rc == WYOMING_OK && text) {
			printf("Run %d: total=%.0fms  post-audio=%.0fms  audio=%.1fs  "
			       "chunks=%d  RTF=%.2f\n"
			       "        text=\"%s\"\n",
			       run + 1, total, post_audio_latency, audio_dur,
			       chunks_sent, total / 1000.0 / audio_dur, text);
			free(text);
		} else {
			printf("Run %d: FAILED (%d)\n", run + 1, rc);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = "127.0.0.1";
	uint16_t port = 10200;
	const char *wav_path = NULL;
	int streaming = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--streaming") == 0)
			streaming = 1;
		else if (!host || i == 1)
			host = argv[i];
		else if (i == 2)
			port = (uint16_t)atoi(argv[i]);
		else if (!wav_path)
			wav_path = argv[i];
	}

	/* Find a test WAV if none specified */
	if (!wav_path) {
		/* Try to use a cached TTS file */
		static const char *candidates[] = {
			"/usr/share/kerchunk/sounds/cache/tts/1f5ca60e_14.wav",
			"/usr/share/kerchunk/sounds/cache/tts/53e98730_91.wav",
			NULL
		};
		for (int i = 0; candidates[i]; i++) {
			if (access(candidates[i], R_OK) == 0) {
				wav_path = candidates[i];
				break;
			}
		}
	}

	if (!wav_path) {
		fprintf(stderr, "Usage: bench_asr [host] [port] [wav_file] [--streaming]\n");
		fprintf(stderr, "No WAV file found — generate one with: kerchunk tts say \"test\"\n");
		return 1;
	}

	int16_t *pcm;
	size_t samples;
	int rate;
	if (read_wav(wav_path, &pcm, &samples, &rate) != 0) {
		fprintf(stderr, "Failed to read WAV: %s\n", wav_path);
		return 1;
	}

	printf("ASR Latency Benchmark\n");
	printf("Server: %s:%d\n", host, port);
	printf("WAV: %s (%zu samples, %d Hz, %.1fs)\n",
	       wav_path, samples, rate, (double)samples / rate);

	int rc = 0;

	if (!streaming || streaming == 0)
		rc = run_batch(host, port, pcm, samples, rate);

	if (streaming)
		rc = run_streaming(host, port, pcm, samples, rate);

	free(pcm);
	return rc;
}
