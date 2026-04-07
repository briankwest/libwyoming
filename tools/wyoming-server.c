/*
 * wyoming-server — Combined TTS + ASR Wyoming protocol server
 *
 * Single daemon serving both text-to-speech (Piper) and automatic
 * speech recognition (Sherpa-ONNX) on one port. Home Assistant and
 * kerchunk connect once and see all capabilities via describe.
 *
 * Usage:
 *   wyoming-server -c /etc/wyoming/wyoming-server.conf
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#include <libwyoming/wyoming.h>
#include "wyoming_config.h"
#include "wyoming_log.h"
#include "cJSON.h"

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
		"Usage: wyoming-server [options]\n"
		"\n"
		"  -c, --config FILE     Config file (default: /etc/wyoming/wyoming-server.conf)\n"
		"  -v, --version         Show version and exit\n"
		"  -h, --help            Show this help\n"
		"\n"
		"Config file format (INI):\n"
		"  [server]\n"
		"  host = 0.0.0.0\n"
		"  port = 10200\n"
		"\n"
		"  [tts]\n"
		"  engine = piper\n"
		"  model = /usr/share/wyoming/voices/en_US-lessac-high/model.onnx\n"
		"\n"
		"  [asr]\n"
		"  engine = sherpa\n"
		"  model_dir = /usr/share/wyoming/models/whisper-base.en\n"
		"  model_type = whisper\n"
		"  language = en\n"
		"\n");
}

/* ── Piper TTS via subprocess (same as wyoming-piper-server) ── */

typedef struct {
	const char *model_path;
	const char *piper_bin;
	int         sample_rate;
} piper_config_t;

static wyoming_error_t piper_tts_callback(
	const char *text, const char *voice, const char *speaker,
	int16_t **pcm_out, size_t *samples_out,
	wyoming_audio_format_t *format_out, void *userdata)
{
	(void)voice;
	piper_config_t *cfg = (piper_config_t *)userdata;

	if (!text || !text[0])
		return WYOMING_ERR_INVAL;

	int pipe_in[2], pipe_out[2];
	if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0)
		return WYOMING_ERR_IO;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipe_in[0]); close(pipe_in[1]);
		close(pipe_out[0]); close(pipe_out[1]);
		return WYOMING_ERR_IO;
	}

	if (pid == 0) {
		/* Child: piper --model X --output_raw --quiet */
		close(pipe_in[1]);
		close(pipe_out[0]);
		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		close(pipe_in[0]);
		close(pipe_out[1]);

		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

		if (speaker && speaker[0]) {
			execlp(cfg->piper_bin, cfg->piper_bin,
			       "--model", cfg->model_path,
			       "--speaker", speaker,
			       "--output_raw", "--quiet", NULL);
		} else {
			execlp(cfg->piper_bin, cfg->piper_bin,
			       "--model", cfg->model_path,
			       "--output_raw", "--quiet", NULL);
		}
		_exit(127);
	}

	/* Parent */
	close(pipe_in[0]);
	close(pipe_out[1]);

	/* Write text to piper stdin */
	write(pipe_in[1], text, strlen(text));
	write(pipe_in[1], "\n", 1);
	close(pipe_in[1]);

	/* Read raw PCM from piper stdout */
	size_t cap = 65536, len = 0;
	uint8_t *buf = malloc(cap);
	if (!buf) { close(pipe_out[0]); waitpid(pid, NULL, 0); return WYOMING_ERR_NOMEM; }

	for (;;) {
		ssize_t n = read(pipe_out[0], buf + len, cap - len);
		if (n <= 0) break;
		len += (size_t)n;
		if (len >= cap) {
			cap *= 2;
			uint8_t *p = realloc(buf, cap);
			if (!p) { free(buf); close(pipe_out[0]); waitpid(pid, NULL, 0); return WYOMING_ERR_NOMEM; }
			buf = p;
		}
	}
	close(pipe_out[0]);

	int status;
	waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || len < 2) {
		free(buf);
		return WYOMING_ERR_PIPER;
	}

	*pcm_out = (int16_t *)buf;
	*samples_out = len / sizeof(int16_t);
	format_out->rate = cfg->sample_rate;
	format_out->width = 2;
	format_out->channels = 1;
	return WYOMING_OK;
}

static int parse_sample_rate(const char *model_path)
{
	char json_path[520];
	snprintf(json_path, sizeof(json_path), "%s.json", model_path);
	FILE *f = fopen(json_path, "r");
	if (!f) return 22050;

	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	const char *sr = strstr(buf, "\"sample_rate\"");
	if (!sr) return 22050;
	sr += strlen("\"sample_rate\"");
	while (*sr && (*sr == ' ' || *sr == ':' || *sr == '\t')) sr++;
	int rate = atoi(sr);
	return (rate >= 8000 && rate <= 48000) ? rate : 22050;
}

static char *extract_voice_name(const char *path)
{
	/* /usr/share/wyoming/voices/en_US-lessac-high/model.onnx → en_US-lessac-high */
	char *p = strdup(path);
	if (!p) return NULL;
	char *slash = strrchr(p, '/');
	if (slash) {
		*slash = '\0';
		char *name = strrchr(p, '/');
		if (name) {
			char *result = strdup(name + 1);
			free(p);
			return result;
		}
	}
	free(p);
	return NULL;
}

static char *extract_language(const char *voice_name)
{
	/* en_US-lessac-high → en_US */
	char *dash = strchr(voice_name, '-');
	if (!dash) return strdup(voice_name);
	size_t len = (size_t)(dash - voice_name);
	char *lang = malloc(len + 1);
	if (!lang) return NULL;
	memcpy(lang, voice_name, len);
	lang[len] = '\0';
	return lang;
}

/* ── Main ── */

int main(int argc, char **argv)
{
	const char *config_path = "/etc/wyoming/wyoming-server.conf";

	static struct option long_opts[] = {
		{ "config",  required_argument, NULL, 'c' },
		{ "version", no_argument,       NULL, 'v' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:vh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c': config_path = optarg; break;
		case 'v':
			printf("wyoming-server %s\n", LIBWYOMING_VERSION_STRING);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}

	/* Load config */
	wyoming_config_t *cfg = wyoming_config_load(config_path);
	if (!cfg) {
		WY_LOGE("cannot read config: %s", config_path);
		return 1;
	}

	/* Initialize logging from config */
	{
		const char *log_level = wyoming_config_get(cfg, "log", "level");
		const char *log_file  = wyoming_config_get(cfg, "log", "file");
		wy_log_init(wy_log_parse_level(log_level), log_file);
	}

	const char *host = wyoming_config_get(cfg, "server", "host");
	int port = wyoming_config_get_int(cfg, "server", "port", 10200);
	if (!host) host = "0.0.0.0";

	WY_LOGI("Wyoming server %s", LIBWYOMING_VERSION_STRING);
	WY_LOGI("config: %s", config_path);

	/* Create server */
	wyoming_server_t *srv = wyoming_server_create(host, (uint16_t)port);
	if (!srv) {
		WY_LOGE("failed to bind %s:%d", host, port);
		wyoming_config_free(cfg);
		return 1;
	}

	/* ── TTS engine ── */

	static piper_config_t piper_cfg;
	const char *tts_engine = wyoming_config_get(cfg, "tts", "engine");
	const char *tts_model = wyoming_config_get(cfg, "tts", "model");

	if (tts_engine && tts_model && strcmp(tts_engine, "piper") == 0) {
		struct stat st;
		if (stat(tts_model, &st) != 0) {
			WY_LOGW("TTS model not found: %s", tts_model);
		} else {
			const char *piper_bin = wyoming_config_get(cfg, "tts", "piper_binary");
			if (!piper_bin) piper_bin = "/usr/bin/piper";

			piper_cfg.model_path = tts_model;
			piper_cfg.piper_bin = piper_bin;
			piper_cfg.sample_rate = parse_sample_rate(tts_model);

			wyoming_server_set_tts(srv, piper_tts_callback, &piper_cfg,
			                       "piper", LIBWYOMING_VERSION_STRING);

			char *voice_name = extract_voice_name(tts_model);
			char *language = extract_language(voice_name ? voice_name : "unknown");
			const char *langs[] = { language, NULL };
			wyoming_server_add_voice(srv, voice_name ? voice_name : "default",
			                         langs, NULL);

			WY_LOGI("TTS: piper voice=%s rate=%d",
			        voice_name ? voice_name : "?", piper_cfg.sample_rate);

			free(voice_name);
			free(language);
		}
	}

#ifdef WYOMING_HAVE_PIPER
	/* Native Piper engine (if available and configured) */
	if (tts_engine && tts_model && strcmp(tts_engine, "piper-native") == 0) {
		const char *espeak_data = wyoming_config_get(cfg, "tts", "espeak_data");
		if (!espeak_data) espeak_data = "/usr/share/espeak-ng-data";

		wyoming_piper_t *piper = wyoming_piper_create(tts_model, espeak_data);
		if (piper) {
			wyoming_server_set_tts(srv, wyoming_piper_get_callback(),
			                       wyoming_piper_as_userdata(piper),
			                       "piper", LIBWYOMING_VERSION_STRING);

			const char *vname = wyoming_piper_voice_name(piper);
			const char *vlang = wyoming_piper_language(piper);
			const char *langs[] = { vlang, NULL };
			wyoming_server_add_voice(srv, vname ? vname : "default", langs, NULL);

			WY_LOGI("TTS: piper-native voice=%s rate=%d",
			        vname ? vname : "?", wyoming_piper_sample_rate(piper));
		} else {
			WY_LOGW("failed to load Piper model: %s", tts_model);
		}
	}
#endif

	/* ── ASR engine ── */

#ifdef WYOMING_HAVE_SHERPA
	const char *asr_engine = wyoming_config_get(cfg, "asr", "engine");
	const char *asr_model_dir = wyoming_config_get(cfg, "asr", "model_dir");

	if (asr_engine && asr_model_dir && strcmp(asr_engine, "sherpa") == 0) {
		const char *model_type = wyoming_config_get(cfg, "asr", "model_type");
		const char *language = wyoming_config_get(cfg, "asr", "language");
		int streaming = wyoming_config_get_bool(cfg, "asr", "streaming", 0);

		if (!model_type) model_type = "whisper";
		if (!language) language = "en";

		wyoming_sherpa_t *sherpa = NULL;
		if (streaming)
			sherpa = wyoming_sherpa_create_streaming(asr_model_dir, model_type, language);
		else
			sherpa = wyoming_sherpa_create(asr_model_dir, model_type, language);

		if (sherpa) {
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

			const char *langs[] = { language, NULL };
			wyoming_server_add_asr_model(srv,
			    wyoming_sherpa_model_name(sherpa), langs,
			    streaming ? "streaming" : "offline");

			WY_LOGI("ASR: sherpa-onnx model=%s type=%s lang=%s %s",
			        wyoming_sherpa_model_name(sherpa), model_type, language,
			        streaming ? "(streaming)" : "(batch)");
		} else {
			WY_LOGW("failed to load ASR model: %s", asr_model_dir);
		}
	}
#endif

	/* Signal handling */
	g_server = srv;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	WY_LOGI("listening on %s:%d", host, port);

	wyoming_error_t rc = wyoming_server_run(srv);

	WY_LOGI("shutting down");

	wyoming_server_destroy(srv);
	wyoming_config_free(cfg);
	wy_log_shutdown();

	return (rc == WYOMING_OK) ? 0 : 1;
}
