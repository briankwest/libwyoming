#include "config.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

/* ── Server structure ────────────────────────────────────────── */

struct wyoming_server {
	int              listen_fd;
	int              stop_pipe[2];
	volatile int     running;

	/* Bind info */
	uint16_t         bound_port;

	/* TTS callback */
	wyoming_tts_fn   tts_fn;
	void            *tts_userdata;
	char            *tts_name;
	char            *tts_version;

	/* Registered voices */
	server_voice_t   voices[WYOMING_MAX_VOICES];
	int              voice_count;

#ifdef HAVE_EPOLL
	int              epoll_fd;
#endif
};

/* ── Create / Destroy ────────────────────────────────────────── */

wyoming_server_t *wyoming_server_create(const char *bind_addr,
                                        uint16_t port)
{
	wyoming_server_t *srv = calloc(1, sizeof(*srv));
	if (!srv) return NULL;

	srv->listen_fd = -1;
	srv->stop_pipe[0] = -1;
	srv->stop_pipe[1] = -1;
#ifdef HAVE_EPOLL
	srv->epoll_fd = -1;
#endif

	/* Create self-pipe for signal-safe stop */
	if (pipe(srv->stop_pipe) < 0) {
		free(srv);
		return NULL;
	}
	/* Make write end non-blocking */
	int flags = fcntl(srv->stop_pipe[1], F_GETFL);
	if (flags >= 0)
		fcntl(srv->stop_pipe[1], F_SETFL, flags | O_NONBLOCK);

	/* Create listen socket */
	srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->listen_fd < 0) {
		close(srv->stop_pipe[0]);
		close(srv->stop_pipe[1]);
		free(srv);
		return NULL;
	}

	int opt = 1;
	setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (!bind_addr || strcmp(bind_addr, "0.0.0.0") == 0)
		addr.sin_addr.s_addr = INADDR_ANY;
	else
		inet_pton(AF_INET, bind_addr, &addr.sin_addr);

	if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(srv->listen_fd);
		close(srv->stop_pipe[0]);
		close(srv->stop_pipe[1]);
		free(srv);
		return NULL;
	}

	/* Query actual bound port */
	struct sockaddr_in bound;
	socklen_t bound_len = sizeof(bound);
	if (getsockname(srv->listen_fd, (struct sockaddr *)&bound, &bound_len) == 0)
		srv->bound_port = ntohs(bound.sin_port);

	if (listen(srv->listen_fd, SOMAXCONN) < 0) {
		close(srv->listen_fd);
		close(srv->stop_pipe[0]);
		close(srv->stop_pipe[1]);
		free(srv);
		return NULL;
	}

	/* Make listen socket non-blocking */
	flags = fcntl(srv->listen_fd, F_GETFL);
	if (flags >= 0)
		fcntl(srv->listen_fd, F_SETFL, flags | O_NONBLOCK);

	return srv;
}

void wyoming_server_set_tts(wyoming_server_t *srv,
                            wyoming_tts_fn fn,
                            void *userdata,
                            const char *name,
                            const char *version)
{
	if (!srv) return;
	srv->tts_fn = fn;
	srv->tts_userdata = userdata;
	free(srv->tts_name);
	srv->tts_name = name ? strdup(name) : NULL;
	free(srv->tts_version);
	srv->tts_version = version ? strdup(version) : NULL;
}

wyoming_error_t wyoming_server_add_voice(wyoming_server_t *srv,
                                         const char *name,
                                         const char *const *languages,
                                         const char *description)
{
	if (!srv || !name)
		return WYOMING_ERR_INVAL;
	if (srv->voice_count >= WYOMING_MAX_VOICES)
		return WYOMING_ERR_NOMEM;

	server_voice_t *v = &srv->voices[srv->voice_count];
	memset(v, 0, sizeof(*v));
	v->name = strdup(name);
	if (description)
		v->description = strdup(description);

	if (languages) {
		for (int i = 0; languages[i] && i < WYOMING_MAX_LANGS; i++) {
			v->languages[i] = strdup(languages[i]);
			v->language_count++;
		}
	}

	srv->voice_count++;
	return WYOMING_OK;
}

wyoming_error_t wyoming_server_add_speaker(wyoming_server_t *srv,
                                           const char *speaker_name)
{
	if (!srv || !speaker_name)
		return WYOMING_ERR_INVAL;
	if (srv->voice_count == 0)
		return WYOMING_ERR_INVAL;

	server_voice_t *v = &srv->voices[srv->voice_count - 1];
	if (v->speaker_count >= WYOMING_MAX_SPEAKERS)
		return WYOMING_ERR_NOMEM;

	v->speakers[v->speaker_count] = strdup(speaker_name);
	v->speaker_count++;
	return WYOMING_OK;
}

uint16_t wyoming_server_port(wyoming_server_t *srv)
{
	return srv ? srv->bound_port : 0;
}

void wyoming_server_stop(wyoming_server_t *srv)
{
	if (!srv) return;
	srv->running = 0;
	/* Wake up epoll/poll via self-pipe */
	if (write(srv->stop_pipe[1], "x", 1) < 0) { /* best effort */ }
}

void wyoming_server_destroy(wyoming_server_t *srv)
{
	if (!srv) return;

	if (srv->listen_fd >= 0) close(srv->listen_fd);
	if (srv->stop_pipe[0] >= 0) close(srv->stop_pipe[0]);
	if (srv->stop_pipe[1] >= 0) close(srv->stop_pipe[1]);
#ifdef HAVE_EPOLL
	if (srv->epoll_fd >= 0) close(srv->epoll_fd);
#endif

	free(srv->tts_name);
	free(srv->tts_version);

	for (int i = 0; i < srv->voice_count; i++) {
		server_voice_t *v = &srv->voices[i];
		free(v->name);
		free(v->description);
		for (int j = 0; j < v->language_count; j++)
			free(v->languages[j]);
		for (int j = 0; j < v->speaker_count; j++)
			free(v->speakers[j]);
	}

	free(srv);
}

/* ── Build info response ─────────────────────────────────────── */

static cJSON *build_info_data(wyoming_server_t *srv)
{
	cJSON *data = cJSON_CreateObject();

	/* Build TTS array */
	cJSON *tts_arr = cJSON_AddArrayToObject(data, "tts");

	if (srv->tts_fn) {
		cJSON *tts = cJSON_CreateObject();
		if (srv->tts_name)
			cJSON_AddStringToObject(tts, "name", srv->tts_name);
		if (srv->tts_version)
			cJSON_AddStringToObject(tts, "version", srv->tts_version);

		cJSON *voices = cJSON_AddArrayToObject(tts, "voices");
		for (int i = 0; i < srv->voice_count; i++) {
			server_voice_t *sv = &srv->voices[i];
			cJSON *voice = cJSON_CreateObject();

			cJSON_AddStringToObject(voice, "name", sv->name);
			if (sv->description)
				cJSON_AddStringToObject(voice, "description",
				                        sv->description);

			cJSON *langs = cJSON_AddArrayToObject(voice, "languages");
			for (int j = 0; j < sv->language_count; j++)
				cJSON_AddItemToArray(langs,
				                     cJSON_CreateString(sv->languages[j]));

			cJSON *speakers = cJSON_AddArrayToObject(voice, "speakers");
			for (int j = 0; j < sv->speaker_count; j++) {
				cJSON *spk = cJSON_CreateObject();
				cJSON_AddStringToObject(spk, "name", sv->speakers[j]);
				cJSON_AddItemToArray(speakers, spk);
			}

			cJSON_AddItemToArray(voices, voice);
		}

		cJSON_AddItemToArray(tts_arr, tts);
	}

	/* Empty arrays for other service types */
	cJSON_AddArrayToObject(data, "asr");
	cJSON_AddArrayToObject(data, "handle");
	cJSON_AddArrayToObject(data, "intent");
	cJSON_AddArrayToObject(data, "wake");

	return data;
}

/* ── Handle one client connection ────────────────────────────── */

#define AUDIO_CHUNK_SAMPLES 4096

static void handle_client(wyoming_server_t *srv, int client_fd)
{
	wyoming_iobuf_t io;
	iobuf_init(&io, client_fd);

	/* Set receive timeout so misbehaving clients don't block forever */
	struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	for (;;) {
		if (!srv->running) break;

		wyoming_event_t event;
		wyoming_event_init(&event);

		wyoming_error_t rc = wyoming_event_read_iobuf(&io, &event);
		if (rc == WYOMING_ERR_EOF)
			break;
		if (rc != WYOMING_OK) {
			wyoming_event_free(&event);
			break;
		}

		/* Dispatch: describe */
		if (strcmp(event.type, WYOMING_EVENT_DESCRIBE) == 0) {
			wyoming_event_free(&event);

			wyoming_event_t resp;
			wyoming_event_init(&resp);
			resp.type = strdup(WYOMING_EVENT_INFO);
			resp.data = build_info_data(srv);

			wyoming_event_write_fd(client_fd, &resp);
			wyoming_event_free(&resp);
			continue;
		}

		/* Dispatch: synthesize */
		if (strcmp(event.type, WYOMING_EVENT_SYNTHESIZE) == 0) {
			const char *text = NULL;
			const char *voice = NULL;
			const char *speaker = NULL;

			if (event.data) {
				cJSON *t = cJSON_GetObjectItemCaseSensitive(
					event.data, "text");
				if (cJSON_IsString(t))
					text = t->valuestring;

				cJSON *v = cJSON_GetObjectItemCaseSensitive(
					event.data, "voice");
				if (cJSON_IsObject(v)) {
					cJSON *vn = cJSON_GetObjectItemCaseSensitive(v, "name");
					if (cJSON_IsString(vn))
						voice = vn->valuestring;
					cJSON *vs = cJSON_GetObjectItemCaseSensitive(v, "speaker");
					if (cJSON_IsString(vs))
						speaker = vs->valuestring;
				}
			}

			if (!text || !srv->tts_fn) {
				wyoming_event_free(&event);
				continue;
			}

			int16_t *pcm = NULL;
			size_t samples = 0;
			wyoming_audio_format_t fmt = { 0 };

			rc = srv->tts_fn(text, voice, speaker,
			                 &pcm, &samples, &fmt,
			                 srv->tts_userdata);

			wyoming_event_free(&event);

			if (rc != WYOMING_OK || !pcm) {
				free(pcm);
				continue;
			}

			/* Send audio-start */
			{
				wyoming_event_t start;
				wyoming_event_init(&start);
				start.type = strdup(WYOMING_EVENT_AUDIO_START);
				start.data = cJSON_CreateObject();
				cJSON_AddNumberToObject(start.data, "rate", fmt.rate);
				cJSON_AddNumberToObject(start.data, "width", fmt.width);
				cJSON_AddNumberToObject(start.data, "channels",
				                        fmt.channels);
				cJSON_AddNumberToObject(start.data, "timestamp", 0);
				wyoming_event_write_fd(client_fd, &start);
				wyoming_event_free(&start);
			}

			/* Send audio-chunks */
			size_t offset = 0;
			while (offset < samples) {
				size_t chunk = samples - offset;
				if (chunk > AUDIO_CHUNK_SAMPLES)
					chunk = AUDIO_CHUNK_SAMPLES;

				wyoming_event_t achunk;
				wyoming_event_init(&achunk);
				achunk.type = strdup(WYOMING_EVENT_AUDIO_CHUNK);
				achunk.data = cJSON_CreateObject();
				cJSON_AddNumberToObject(achunk.data, "rate", fmt.rate);
				cJSON_AddNumberToObject(achunk.data, "width", fmt.width);
				cJSON_AddNumberToObject(achunk.data, "channels",
				                        fmt.channels);

				achunk.payload = (uint8_t *)(pcm + offset);
				achunk.payload_len = chunk * sizeof(int16_t);

				wyoming_event_write_fd(client_fd, &achunk);

				/* Don't free payload — it's borrowed from pcm */
				achunk.payload = NULL;
				achunk.payload_len = 0;
				wyoming_event_free(&achunk);

				offset += chunk;
			}

			/* Send audio-stop */
			{
				wyoming_event_t stop;
				wyoming_event_init(&stop);
				stop.type = strdup(WYOMING_EVENT_AUDIO_STOP);
				wyoming_event_write_fd(client_fd, &stop);
				wyoming_event_free(&stop);
			}

			free(pcm);
			continue;
		}

		/* Unknown event — ignore */
		wyoming_event_free(&event);
	}
}

/* ── Event loop ──────────────────────────────────────────────── */

#ifdef HAVE_EPOLL

wyoming_error_t wyoming_server_run(wyoming_server_t *srv)
{
	if (!srv) return WYOMING_ERR_INVAL;

	/* Ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);

	srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (srv->epoll_fd < 0)
		return WYOMING_ERR_IO;

	struct epoll_event ev;

	/* Add listen fd */
	ev.events = EPOLLIN;
	ev.data.fd = srv->listen_fd;
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

	/* Add stop pipe */
	ev.events = EPOLLIN;
	ev.data.fd = srv->stop_pipe[0];
	epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->stop_pipe[0], &ev);

	srv->running = 1;

	while (srv->running) {
		struct epoll_event events[8];
		int n = epoll_wait(srv->epoll_fd, events, 8, 1000);

		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		for (int i = 0; i < n && srv->running; i++) {
			if (events[i].data.fd == srv->stop_pipe[0]) {
				srv->running = 0;
				break;
			}

			if (events[i].data.fd == srv->listen_fd) {
				struct sockaddr_in client_addr;
				socklen_t addr_len = sizeof(client_addr);
				int client_fd = accept(srv->listen_fd,
				                       (struct sockaddr *)&client_addr,
				                       &addr_len);
				if (client_fd < 0)
					continue;

				int flag = 1;
				setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
				           &flag, sizeof(flag));

				handle_client(srv, client_fd);
				close(client_fd);
			}
		}
	}

	close(srv->epoll_fd);
	srv->epoll_fd = -1;
	return WYOMING_OK;
}

#else /* poll() fallback */

wyoming_error_t wyoming_server_run(wyoming_server_t *srv)
{
	if (!srv) return WYOMING_ERR_INVAL;

	signal(SIGPIPE, SIG_IGN);

	srv->running = 1;

	while (srv->running) {
		struct pollfd fds[2];
		fds[0].fd = srv->listen_fd;
		fds[0].events = POLLIN;
		fds[1].fd = srv->stop_pipe[0];
		fds[1].events = POLLIN;

		int n = poll(fds, 2, 1000);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if (fds[1].revents & POLLIN) {
			srv->running = 0;
			break;
		}

		if (fds[0].revents & POLLIN) {
			struct sockaddr_in client_addr;
			socklen_t addr_len = sizeof(client_addr);
			int client_fd = accept(srv->listen_fd,
			                       (struct sockaddr *)&client_addr,
			                       &addr_len);
			if (client_fd < 0)
				continue;

			int flag = 1;
			setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
			           &flag, sizeof(flag));

			handle_client(srv, client_fd);
			close(client_fd);
		}
	}

	return WYOMING_OK;
}

#endif /* HAVE_EPOLL */
