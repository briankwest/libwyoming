/*
 * wyoming-describe — Query a Wyoming server's capabilities
 *
 * Connects to a Wyoming server and prints what services (TTS, ASR)
 * it offers, including available voices and models.
 *
 * Usage:
 *   wyoming-describe localhost 10200
 *   wyoming-describe 192.168.1.5 10300
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwyoming/wyoming.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: wyoming-describe HOST PORT\n");
		return 1;
	}

	const char *host = argv[1];
	uint16_t port = (uint16_t)atoi(argv[2]);

	wyoming_conn_t *conn = wyoming_connect(host, port);
	if (!conn) {
		fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
		return 1;
	}

	wyoming_info_t info = {0};
	wyoming_error_t rc = wyoming_describe(conn, &info);
	wyoming_close(conn);

	if (rc != WYOMING_OK) {
		fprintf(stderr, "Describe failed: %d\n", rc);
		return 1;
	}

	printf("Wyoming server at %s:%d\n\n", host, port);

	/* TTS */
	if (info.tts_count > 0) {
		printf("TTS engines: %d\n", info.tts_count);
		for (int i = 0; i < info.tts_count; i++) {
			wyoming_tts_info_t *tts = &info.tts[i];
			printf("  %s", tts->name ? tts->name : "?");
			if (tts->version) printf(" v%s", tts->version);
			printf("\n");

			for (int j = 0; j < tts->voice_count; j++) {
				wyoming_voice_t *v = &tts->voices[j];
				printf("    voice: %s", v->name ? v->name : "?");
				if (v->description) printf(" (%s)", v->description);
				if (v->language_count > 0) {
					printf(" [");
					for (int k = 0; k < v->language_count; k++) {
						if (k > 0) printf(", ");
						printf("%s", v->languages[k]);
					}
					printf("]");
				}
				if (v->speaker_count > 0)
					printf(" (%d speakers)", v->speaker_count);
				printf("\n");
			}
		}
		printf("\n");
	} else {
		printf("TTS: none\n\n");
	}

	/* ASR */
	if (info.asr_count > 0) {
		printf("ASR engines: %d\n", info.asr_count);
		for (int i = 0; i < info.asr_count; i++) {
			wyoming_asr_info_t *asr = &info.asr[i];
			printf("  %s", asr->name ? asr->name : "?");
			if (asr->version) printf(" v%s", asr->version);
			printf("\n");

			for (int j = 0; j < asr->model_count; j++) {
				wyoming_model_t *m = &asr->models[j];
				printf("    model: %s", m->name ? m->name : "?");
				if (m->description) printf(" (%s)", m->description);
				if (m->language_count > 0) {
					printf(" [");
					for (int k = 0; k < m->language_count; k++) {
						if (k > 0) printf(", ");
						printf("%s", m->languages[k]);
					}
					printf("]");
				}
				printf("\n");
			}
		}
		printf("\n");
	} else {
		printf("ASR: none\n\n");
	}

	wyoming_info_free(&info);
	return 0;
}
