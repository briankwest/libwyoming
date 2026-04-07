/*
 * wyoming-asr-server — Standalone Wyoming ASR server using Sherpa-ONNX
 *
 * Drop-in replacement for wyoming-faster-whisper (Python).
 * Supports batch and streaming ASR over the Wyoming protocol.
 *
 * Usage:
 *   wyoming-asr-server --model-dir /path/to/model --port 10300
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

#include <libwyoming/wyoming.h>

static wyoming_server_t *g_server = NULL;

static void handle_signal(int sig)
{
	(void)sig;
	if (g_server)
		wyoming_server_stop(g_server);
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: wyoming-asr-server [options]\n"
		"\n"
		"  --model-dir DIR       Path to Sherpa-ONNX model directory (required)\n"
		"  --model-type TYPE     Model type: whisper, zipformer, paraformer, nemo_ctc,\n"
		"                        sense_voice (default: whisper)\n"
		"  --port PORT           Listen port (default: 10300)\n"
		"  --host HOST           Bind address (default: 0.0.0.0)\n"
		"  --language LANG       Language code, e.g. en (default: auto)\n"
		"  --streaming           Enable streaming ASR (requires streaming model)\n"
		"  --version             Show version and exit\n"
		"  --help                Show this help\n"
		"\n"
		"Model types:\n"
		"  whisper      Offline (batch) Whisper models — best accuracy\n"
		"  zipformer    Online (streaming) transducer — low latency\n"
		"  paraformer   Online (streaming) or offline\n"
		"  nemo_ctc     Online (streaming) NeMo CTC\n"
		"  sense_voice  Offline SenseVoice — multilingual\n"
		"\n"
		"Example:\n"
		"  wyoming-asr-server --model-dir /usr/share/wyoming/models/whisper-base.en \\\n"
		"                     --model-type whisper --language en --port 10300\n"
		"\n");
}

int main(int argc, char **argv)
{
	const char *model_dir  = NULL;
	const char *model_type = "whisper";
	const char *host       = "0.0.0.0";
	const char *language   = NULL;
	int         port       = 10300;
	int         streaming  = 0;

	static struct option long_opts[] = {
		{ "model-dir",   required_argument, NULL, 'm' },
		{ "model-type",  required_argument, NULL, 't' },
		{ "port",        required_argument, NULL, 'p' },
		{ "host",        required_argument, NULL, 'H' },
		{ "language",    required_argument, NULL, 'l' },
		{ "streaming",   no_argument,       NULL, 's' },
		{ "version",     no_argument,       NULL, 'v' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "m:t:p:H:l:svh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm': model_dir = optarg; break;
		case 't': model_type = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'H': host = optarg; break;
		case 'l': language = optarg; break;
		case 's': streaming = 1; break;
		case 'v':
			printf("wyoming-asr-server %s (sherpa-onnx)\n", LIBWYOMING_VERSION_STRING);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}

	if (!model_dir || port <= 0 || port > 65535) {
		fprintf(stderr, "Error: --model-dir and valid --port are required\n\n");
		usage(stderr);
		return 1;
	}

#ifndef WYOMING_HAVE_SHERPA
	fprintf(stderr, "Error: this build does not include Sherpa-ONNX support\n");
	return 1;
#else

	/* Create recognizer */
	wyoming_sherpa_t *sherpa = NULL;

	if (streaming) {
		fprintf(stderr, "Creating streaming %s recognizer...\n", model_type);
		sherpa = wyoming_sherpa_create_streaming(model_dir, model_type, language);
	} else {
		fprintf(stderr, "Creating offline %s recognizer...\n", model_type);
		sherpa = wyoming_sherpa_create(model_dir, model_type, language);
	}

	if (!sherpa) {
		fprintf(stderr, "Error: failed to create recognizer from %s\n", model_dir);
		fprintf(stderr, "Check model files (encoder.onnx, decoder.onnx, tokens.txt)\n");
		return 1;
	}

	fprintf(stderr, "Model: %s (%s, %d Hz)\n",
	        wyoming_sherpa_model_name(sherpa),
	        wyoming_sherpa_language(sherpa),
	        wyoming_sherpa_sample_rate(sherpa));

	/* Create server */
	wyoming_server_t *srv = wyoming_server_create(host, (uint16_t)port);
	if (!srv) {
		fprintf(stderr, "Error: failed to bind %s:%d\n", host, port);
		wyoming_sherpa_destroy(sherpa);
		return 1;
	}

	/* Register ASR engine */
	wyoming_server_set_asr(srv,
	                        wyoming_sherpa_get_asr_callback(),
	                        wyoming_sherpa_as_userdata(sherpa),
	                        "sherpa-onnx",
	                        LIBWYOMING_VERSION_STRING);

	if (streaming) {
		wyoming_server_set_asr_streaming(srv,
		                                  wyoming_sherpa_get_stream_create(),
		                                  wyoming_sherpa_get_stream_process(),
		                                  wyoming_sherpa_get_stream_destroy(),
		                                  wyoming_sherpa_as_userdata(sherpa));
	}

	/* Register model metadata */
	const char *langs[] = { language ? language : "auto", NULL };
	wyoming_server_add_asr_model(srv,
	                              wyoming_sherpa_model_name(sherpa),
	                              langs,
	                              streaming ? "streaming" : "offline");

	/* Signal handling */
	g_server = srv;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr, "Listening on %s:%d (%s)\n",
	        host, port, streaming ? "streaming" : "batch");

	wyoming_error_t rc = wyoming_server_run(srv);

	fprintf(stderr, "Shutting down...\n");

	wyoming_server_destroy(srv);
	wyoming_sherpa_destroy(sherpa);

	return (rc == WYOMING_OK) ? 0 : 1;

#endif /* WYOMING_HAVE_SHERPA */
}
