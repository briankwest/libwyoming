/*
 * wyoming-speak — Synthesize text to a WAV file via Wyoming TTS
 *
 * Connects to a Wyoming TTS server, synthesizes the given text,
 * and writes the result as a 16-bit mono WAV file.
 *
 * Usage:
 *   wyoming-speak HOST PORT "Hello world" output.wav
 *   wyoming-speak localhost 10200 "The time is now" /tmp/time.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libwyoming/wyoming.h>

/* Write a minimal 16-bit mono PCM WAV file */
static int write_wav(const char *path, const int16_t *pcm, size_t samples,
                     int sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }

    uint32_t data_size = (uint32_t)(samples * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t byte_rate = (uint32_t)(sample_rate * channels * bits / 8);
    uint16_t block_align = (uint16_t)(channels * bits / 8);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1; /* PCM */
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    uint32_t sr = (uint32_t)sample_rate;
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(pcm, sizeof(int16_t), samples, f);

    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: wyoming-speak HOST PORT \"text\" output.wav\n");
        return 1;
    }

    const char *host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *text = argv[3];
    const char *outpath = argv[4];

    wyoming_conn_t *conn = wyoming_connect(host, port);
    if (!conn) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
        return 1;
    }

    int16_t *pcm = NULL;
    size_t samples = 0;
    wyoming_audio_format_t fmt = {0};

    wyoming_error_t rc = wyoming_synthesize_pcm(conn, text, NULL,
                                                 &pcm, &samples, &fmt);
    wyoming_close(conn);

    if (rc != WYOMING_OK) {
        fprintf(stderr, "Synthesize failed: %d\n", rc);
        return 1;
    }

    if (!pcm || samples == 0) {
        fprintf(stderr, "No audio returned\n");
        return 1;
    }

    int ret = write_wav(outpath, pcm, samples, fmt.rate);
    free(pcm);

    if (ret == 0) {
        fprintf(stderr, "Wrote %s (%zu samples, %d Hz)\n",
                outpath, samples, fmt.rate);
    }

    return ret;
}
