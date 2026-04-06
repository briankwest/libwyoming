#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libwyoming/wyoming.h>
#include "cJSON.h"

static wyoming_server_t *g_server = NULL;

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
	const char *model_path;
	const char *piper_bin;
	int         sample_rate;
} piper_config_t;

/* ── TTS callback: runs piper as subprocess ──────────────────── */

static wyoming_error_t piper_subprocess_tts(
	const char *text,
	const char *voice,
	const char *speaker,
	int16_t **pcm_out,
	size_t *samples_out,
	wyoming_audio_format_t *format_out,
	void *userdata)
{
	(void)voice;
	piper_config_t *cfg = (piper_config_t *)userdata;

	if (!text || !text[0])
		return WYOMING_ERR_INVAL;

	/* Build piper command */
	/* piper --model X --output_raw --quiet [--speaker N] */
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
		/* Child: piper process */
		close(pipe_in[1]);   /* close write end of stdin pipe */
		close(pipe_out[0]);  /* close read end of stdout pipe */

		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		close(pipe_in[0]);
		close(pipe_out[1]);

		/* Redirect stderr to /dev/null in quiet mode */
		int devnull = open("/dev/null", 0);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		if (speaker) {
			execlp(cfg->piper_bin, cfg->piper_bin,
			       "--model", cfg->model_path,
			       "--output_raw", "--quiet",
			       "--speaker", speaker,
			       (char *)NULL);
		} else {
			execlp(cfg->piper_bin, cfg->piper_bin,
			       "--model", cfg->model_path,
			       "--output_raw", "--quiet",
			       (char *)NULL);
		}
		_exit(127);
	}

	/* Parent */
	close(pipe_in[0]);   /* close read end of stdin pipe */
	close(pipe_out[1]);  /* close write end of stdout pipe */

	/* Write text to piper's stdin, then close */
	size_t text_len = strlen(text);
	if (write(pipe_in[1], text, text_len) < 0) { /* best effort */ }
	if (write(pipe_in[1], "\n", 1) < 0) { /* best effort */ }
	close(pipe_in[1]);

	/* Read raw PCM from piper's stdout */
	size_t capacity = (size_t)cfg->sample_rate * 4; /* ~4 seconds initial */
	size_t total = 0;
	int16_t *pcm = malloc(capacity * sizeof(int16_t));
	if (!pcm) {
		close(pipe_out[0]);
		waitpid(pid, NULL, 0);
		return WYOMING_ERR_NOMEM;
	}

	for (;;) {
		if (total >= capacity) {
			capacity *= 2;
			int16_t *tmp = realloc(pcm, capacity * sizeof(int16_t));
			if (!tmp) {
				free(pcm);
				close(pipe_out[0]);
				waitpid(pid, NULL, 0);
				return WYOMING_ERR_NOMEM;
			}
			pcm = tmp;
		}

		size_t avail = (capacity - total) * sizeof(int16_t);
		ssize_t n = read(pipe_out[0], (uint8_t *)pcm + total * sizeof(int16_t), avail);
		if (n > 0) {
			total += (size_t)n / sizeof(int16_t);
		} else if (n == 0) {
			break; /* EOF — piper done */
		} else {
			if (errno == EINTR) continue;
			break;
		}
	}

	close(pipe_out[0]);

	int status;
	waitpid(pid, &status, 0);

	if (total == 0) {
		free(pcm);
		return WYOMING_ERR_PIPER;
	}

	*pcm_out = pcm;
	*samples_out = total;
	format_out->rate = cfg->sample_rate;
	format_out->width = 2;
	format_out->channels = 1;

	return WYOMING_OK;
}

/* ── Signal handling ─────────────────────────────────────────── */

static void handle_signal(int sig)
{
	(void)sig;
	if (g_server)
		wyoming_server_stop(g_server);
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: wyoming-piper-server [OPTIONS]\n"
		"\n"
		"Required:\n"
		"  --model PATH          Path to Piper .onnx model file\n"
		"  --port PORT           TCP port to listen on\n"
		"\n"
		"Optional:\n"
		"  --host ADDR           Bind address (default: 0.0.0.0)\n"
		"  --piper PATH          Path to piper binary (default: piper)\n"
		"  --version             Print version and exit\n"
		"  --help                Print this help and exit\n"
	);
}

/* ── Helper: parse sample_rate from .onnx.json ───────────────── */

static int parse_sample_rate(const char *model_path)
{
	size_t mlen = strlen(model_path);
	char *config_path = malloc(mlen + 6);
	if (!config_path) return 22050;
	snprintf(config_path, mlen + 6, "%s.json", model_path);

	FILE *f = fopen(config_path, "rb");
	free(config_path);
	if (!f) return 22050;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) { fclose(f); return 22050; }

	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return 22050; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	(void)rd;
	buf[sz] = '\0';
	fclose(f);

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) return 22050;

	int rate = 22050;
	cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
	if (audio) {
		cJSON *sr = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
		if (cJSON_IsNumber(sr) && sr->valuedouble > 0)
			rate = (int)sr->valuedouble;
	}
	cJSON_Delete(root);
	return rate;
}

/* ── Helper: extract voice name from path ────────────────────── */

static char *extract_voice_name(const char *path)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	const char *dot = strstr(base, ".onnx");
	size_t len = dot ? (size_t)(dot - base) : strlen(base);
	char *name = malloc(len + 1);
	if (name) { memcpy(name, base, len); name[len] = '\0'; }
	return name;
}

/* ── Helper: extract language from voice name ────────────────── */

static char *extract_language(const char *voice_name)
{
	/* "en_US-lessac-high" -> "en_US" */
	const char *dash = strchr(voice_name, '-');
	size_t len = dash ? (size_t)(dash - voice_name) : strlen(voice_name);
	char *lang = malloc(len + 1);
	if (lang) { memcpy(lang, voice_name, len); lang[len] = '\0'; }
	return lang;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	const char *model_path = NULL;
	const char *host = "0.0.0.0";
	const char *piper_bin = "piper";
	int port = 0;

	static struct option long_opts[] = {
		{ "model",   required_argument, NULL, 'm' },
		{ "port",    required_argument, NULL, 'p' },
		{ "host",    required_argument, NULL, 'h' },
		{ "piper",   required_argument, NULL, 'b' },
		{ "version", no_argument,       NULL, 'v' },
		{ "help",    no_argument,       NULL, '?' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "m:p:h:b:v", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm': model_path = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'h': host = optarg; break;
		case 'b': piper_bin = optarg; break;
		case 'v':
			printf("wyoming-piper-server %s\n", LIBWYOMING_VERSION_STRING);
			return 0;
		default:
			usage(opt == '?' ? stdout : stderr);
			return opt == '?' ? 0 : 1;
		}
	}

	if (!model_path || port <= 0 || port > 65535) {
		fprintf(stderr, "Error: --model and --port are required\n\n");
		usage(stderr);
		return 1;
	}

	/* Verify model file exists */
	struct stat st;
	if (stat(model_path, &st) != 0) {
		fprintf(stderr, "Error: model file not found: %s\n", model_path);
		return 1;
	}

	/* Parse config */
	int sample_rate = parse_sample_rate(model_path);
	char *voice_name = extract_voice_name(model_path);
	char *language = extract_language(voice_name ? voice_name : "unknown");

	fprintf(stderr, "Voice: %s (%s, %d Hz)\n",
	        voice_name ? voice_name : "unknown",
	        language ? language : "?",
	        sample_rate);
	fprintf(stderr, "Piper: %s\n", piper_bin);

	/* Configure TTS engine */
	piper_config_t cfg = {
		.model_path  = model_path,
		.piper_bin   = piper_bin,
		.sample_rate = sample_rate,
	};

	/* Create server */
	wyoming_server_t *srv = wyoming_server_create(host, (uint16_t)port);
	if (!srv) {
		fprintf(stderr, "Error: failed to bind %s:%d\n", host, port);
		free(voice_name);
		free(language);
		return 1;
	}

	wyoming_server_set_tts(srv, piper_subprocess_tts, &cfg,
	                       "piper", LIBWYOMING_VERSION_STRING);

	const char *langs[] = { language, NULL };
	wyoming_server_add_voice(srv, voice_name ? voice_name : "default",
	                         langs, NULL);

	/* Signal handling */
	g_server = srv;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr, "Listening on %s:%d\n", host, port);

	wyoming_error_t rc = wyoming_server_run(srv);

	fprintf(stderr, "Shutting down...\n");

	wyoming_server_destroy(srv);
	free(voice_name);
	free(language);

	return (rc == WYOMING_OK) ? 0 : 1;
}
