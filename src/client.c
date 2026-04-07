#include "config.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

/* ── Connect ─────────────────────────────────────────────────── */

wyoming_conn_t *wyoming_connect(const char *host, uint16_t port)
{
	if (!host) return NULL;

	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%u", port);

	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res = NULL;

	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return NULL;

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return NULL;
	}

	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		close(fd);
		freeaddrinfo(res);
		return NULL;
	}
	freeaddrinfo(res);

	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	wyoming_conn_t *conn = calloc(1, sizeof(*conn));
	if (!conn) {
		close(fd);
		return NULL;
	}
	conn->fd = fd;
	iobuf_init(&conn->iobuf, fd);

	return conn;
}

/* ── Read / Write event wrappers ─────────────────────────────── */

wyoming_error_t wyoming_read_event(wyoming_conn_t *conn,
                                   wyoming_event_t *event)
{
	if (!conn || !event)
		return WYOMING_ERR_INVAL;
	return wyoming_event_read_iobuf(&conn->iobuf, event);
}

wyoming_error_t wyoming_write_event(wyoming_conn_t *conn,
                                    const wyoming_event_t *event)
{
	if (!conn || !event)
		return WYOMING_ERR_INVAL;
	return wyoming_event_write_fd(conn->fd, event);
}

/* ── Synthesize (send request) ───────────────────────────────── */

wyoming_error_t wyoming_synthesize(wyoming_conn_t *conn,
                                   const char *text,
                                   const char *voice)
{
	if (!conn || !text)
		return WYOMING_ERR_INVAL;

	cJSON *data = cJSON_CreateObject();
	if (!data) return WYOMING_ERR_NOMEM;

	cJSON_AddStringToObject(data, "text", text);

	if (voice) {
		cJSON *voice_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(voice_obj, "name", voice);
		cJSON_AddItemToObject(data, "voice", voice_obj);
	}

	wyoming_event_t event;
	wyoming_event_init(&event);
	event.type = strdup(WYOMING_EVENT_SYNTHESIZE);
	event.data = data;

	wyoming_error_t rc = wyoming_event_write_fd(conn->fd, &event);
	wyoming_event_free(&event);
	return rc;
}

/* ── Synthesize PCM (high-level: send + collect all audio) ──── */

wyoming_error_t wyoming_synthesize_pcm(wyoming_conn_t *conn,
                                       const char *text,
                                       const char *voice,
                                       int16_t **pcm_out,
                                       size_t *samples_out,
                                       wyoming_audio_format_t *format_out)
{
	if (!conn || !text || !pcm_out || !samples_out || !format_out)
		return WYOMING_ERR_INVAL;

	*pcm_out = NULL;
	*samples_out = 0;
	memset(format_out, 0, sizeof(*format_out));

	/* Send synthesize request */
	wyoming_error_t rc = wyoming_synthesize(conn, text, voice);
	if (rc != WYOMING_OK)
		return rc;

	/* Collect response events */
	int16_t *pcm = NULL;
	size_t   capacity = 0;
	size_t   total = 0;
	int      got_start = 0;

	for (;;) {
		wyoming_event_t event;
		wyoming_event_init(&event);

		rc = wyoming_read_event(conn, &event);
		if (rc != WYOMING_OK) {
			free(pcm);
			return rc;
		}

		if (strcmp(event.type, WYOMING_EVENT_AUDIO_START) == 0) {
			got_start = 1;
			if (event.data) {
				cJSON *r = cJSON_GetObjectItemCaseSensitive(event.data, "rate");
				cJSON *w = cJSON_GetObjectItemCaseSensitive(event.data, "width");
				cJSON *c = cJSON_GetObjectItemCaseSensitive(event.data, "channels");
				if (cJSON_IsNumber(r)) format_out->rate = (int)r->valuedouble;
				if (cJSON_IsNumber(w)) format_out->width = (int)w->valuedouble;
				if (cJSON_IsNumber(c)) format_out->channels = (int)c->valuedouble;
			}
			wyoming_event_free(&event);
			continue;
		}

		if (strcmp(event.type, WYOMING_EVENT_AUDIO_CHUNK) == 0) {
			if (!got_start) {
				wyoming_event_free(&event);
				free(pcm);
				return WYOMING_ERR_PROTO;
			}
			if (event.payload && event.payload_len > 0) {
				size_t chunk_samples = event.payload_len / sizeof(int16_t);
				if (total + chunk_samples > capacity) {
					capacity = (total + chunk_samples) * 2;
					if (capacity < 4096) capacity = 4096;
					int16_t *tmp = realloc(pcm,
					                       capacity * sizeof(int16_t));
					if (!tmp) {
						wyoming_event_free(&event);
						free(pcm);
						return WYOMING_ERR_NOMEM;
					}
					pcm = tmp;
				}
				memcpy(pcm + total, event.payload,
				       chunk_samples * sizeof(int16_t));
				total += chunk_samples;
			}
			wyoming_event_free(&event);
			continue;
		}

		if (strcmp(event.type, WYOMING_EVENT_AUDIO_STOP) == 0) {
			wyoming_event_free(&event);
			break;
		}

		if (strcmp(event.type, WYOMING_EVENT_ERROR) == 0) {
			wyoming_event_free(&event);
			free(pcm);
			return WYOMING_ERR_PROTO;
		}

		/* Unknown event — skip */
		wyoming_event_free(&event);
	}

	*pcm_out = pcm;
	*samples_out = total;
	return WYOMING_OK;
}

/* ── Describe ────────────────────────────────────────────────── */

static void parse_voices(cJSON *voices_arr, wyoming_tts_info_t *tts)
{
	int count = cJSON_GetArraySize(voices_arr);
	if (count <= 0) return;

	tts->voices = calloc((size_t)count, sizeof(wyoming_voice_t));
	if (!tts->voices) return;
	tts->voice_count = count;

	int idx = 0;
	cJSON *v;
	cJSON_ArrayForEach(v, voices_arr) {
		wyoming_voice_t *voice = &tts->voices[idx++];

		cJSON *n = cJSON_GetObjectItemCaseSensitive(v, "name");
		if (cJSON_IsString(n))
			voice->name = strdup(n->valuestring);

		cJSON *desc = cJSON_GetObjectItemCaseSensitive(v, "description");
		if (cJSON_IsString(desc))
			voice->description = strdup(desc->valuestring);

		cJSON *langs = cJSON_GetObjectItemCaseSensitive(v, "languages");
		if (cJSON_IsArray(langs)) {
			int lc = cJSON_GetArraySize(langs);
			if (lc > 0) {
				voice->languages = calloc((size_t)lc, sizeof(char *));
				if (voice->languages) {
					voice->language_count = lc;
					int li = 0;
					cJSON *l;
					cJSON_ArrayForEach(l, langs) {
						if (cJSON_IsString(l))
							voice->languages[li] = strdup(l->valuestring);
						li++;
					}
				}
			}
		}

		cJSON *spks = cJSON_GetObjectItemCaseSensitive(v, "speakers");
		if (cJSON_IsArray(spks)) {
			int sc = cJSON_GetArraySize(spks);
			if (sc > 0) {
				voice->speakers = calloc((size_t)sc,
				                         sizeof(wyoming_speaker_t));
				if (voice->speakers) {
					voice->speaker_count = sc;
					int si = 0;
					cJSON *s;
					cJSON_ArrayForEach(s, spks) {
						cJSON *sn = cJSON_GetObjectItemCaseSensitive(s, "name");
						if (cJSON_IsString(sn))
							voice->speakers[si].name = strdup(sn->valuestring);
						si++;
					}
				}
			}
		}
	}
}

wyoming_error_t wyoming_describe(wyoming_conn_t *conn,
                                 wyoming_info_t *info_out)
{
	if (!conn || !info_out)
		return WYOMING_ERR_INVAL;

	memset(info_out, 0, sizeof(*info_out));

	/* Send describe event */
	wyoming_event_t desc_event;
	wyoming_event_init(&desc_event);
	desc_event.type = strdup(WYOMING_EVENT_DESCRIBE);

	wyoming_error_t rc = wyoming_event_write_fd(conn->fd, &desc_event);
	wyoming_event_free(&desc_event);
	if (rc != WYOMING_OK)
		return rc;

	/* Read info response */
	wyoming_event_t info_event;
	wyoming_event_init(&info_event);

	rc = wyoming_read_event(conn, &info_event);
	if (rc != WYOMING_OK)
		return rc;

	if (strcmp(info_event.type, WYOMING_EVENT_INFO) != 0) {
		wyoming_event_free(&info_event);
		return WYOMING_ERR_PROTO;
	}

	/* Parse TTS info from data */
	if (info_event.data) {
		cJSON *tts_arr = cJSON_GetObjectItemCaseSensitive(
			info_event.data, "tts");
		if (cJSON_IsArray(tts_arr)) {
			int tc = cJSON_GetArraySize(tts_arr);
			if (tc > 0) {
				info_out->tts = calloc((size_t)tc,
				                       sizeof(wyoming_tts_info_t));
				if (info_out->tts) {
					info_out->tts_count = tc;
					int ti = 0;
					cJSON *t;
					cJSON_ArrayForEach(t, tts_arr) {
						wyoming_tts_info_t *tts = &info_out->tts[ti++];

						cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "name");
						if (cJSON_IsString(n))
							tts->name = strdup(n->valuestring);

						cJSON *ver = cJSON_GetObjectItemCaseSensitive(t, "version");
						if (cJSON_IsString(ver))
							tts->version = strdup(ver->valuestring);

						cJSON *voices = cJSON_GetObjectItemCaseSensitive(t, "voices");
						if (cJSON_IsArray(voices))
							parse_voices(voices, tts);
					}
				}
			}
		}
	}

	wyoming_event_free(&info_event);
	return WYOMING_OK;
}

/* ── Info free ───────────────────────────────────────────────── */

void wyoming_info_free(wyoming_info_t *info)
{
	if (!info) return;
	for (int i = 0; i < info->tts_count; i++) {
		wyoming_tts_info_t *tts = &info->tts[i];
		free(tts->name);
		free(tts->version);
		for (int j = 0; j < tts->voice_count; j++) {
			wyoming_voice_t *v = &tts->voices[j];
			free(v->name);
			free(v->description);
			for (int k = 0; k < v->language_count; k++)
				free(v->languages[k]);
			free(v->languages);
			for (int k = 0; k < v->speaker_count; k++)
				free(v->speakers[k].name);
			free(v->speakers);
		}
		free(tts->voices);
	}
	free(info->tts);
	memset(info, 0, sizeof(*info));
}

/* ── Transcribe (ASR) ───────────────────────────────────────── */

wyoming_error_t wyoming_transcribe_pcm(wyoming_conn_t *conn,
                                       const int16_t *pcm,
                                       size_t samples,
                                       const wyoming_audio_format_t *format,
                                       const char *language,
                                       char **text_out)
{
	if (!conn || !pcm || samples == 0 || !format || !text_out)
		return WYOMING_ERR_INVAL;

	*text_out = NULL;
	wyoming_error_t rc;

	/* 1. Send transcribe event (with optional language) */
	{
		wyoming_event_t evt;
		wyoming_event_init(&evt);
		evt.type = strdup(WYOMING_EVENT_TRANSCRIBE);

		cJSON *data = cJSON_CreateObject();
		if (language && language[0])
			cJSON_AddStringToObject(data, "language", language);
		evt.data = data;

		rc = wyoming_write_event(conn, &evt);
		wyoming_event_free(&evt);
		if (rc != WYOMING_OK)
			return rc;
	}

	/* 2. Send audio-start */
	{
		wyoming_event_t evt;
		wyoming_event_init(&evt);
		evt.type = strdup(WYOMING_EVENT_AUDIO_START);

		cJSON *data = cJSON_CreateObject();
		cJSON_AddNumberToObject(data, "rate", format->rate);
		cJSON_AddNumberToObject(data, "width", format->width);
		cJSON_AddNumberToObject(data, "channels", format->channels);
		evt.data = data;

		rc = wyoming_write_event(conn, &evt);
		wyoming_event_free(&evt);
		if (rc != WYOMING_OK)
			return rc;
	}

	/* 3. Send audio-chunks (2048 bytes per chunk) */
	{
		const size_t chunk_samples = 1024;
		const uint8_t *raw = (const uint8_t *)pcm;
		size_t total_bytes = samples * sizeof(int16_t);
		size_t chunk_bytes = chunk_samples * sizeof(int16_t);
		size_t offset = 0;

		while (offset < total_bytes) {
			size_t send_bytes = total_bytes - offset;
			if (send_bytes > chunk_bytes) send_bytes = chunk_bytes;

			wyoming_event_t evt;
			wyoming_event_init(&evt);
			evt.type = strdup(WYOMING_EVENT_AUDIO_CHUNK);

			cJSON *data = cJSON_CreateObject();
			cJSON_AddNumberToObject(data, "rate", format->rate);
			cJSON_AddNumberToObject(data, "width", format->width);
			cJSON_AddNumberToObject(data, "channels", format->channels);
			evt.data = data;

			evt.payload = malloc(send_bytes);
			if (!evt.payload) {
				wyoming_event_free(&evt);
				return WYOMING_ERR_NOMEM;
			}
			memcpy(evt.payload, raw + offset, send_bytes);
			evt.payload_len = send_bytes;

			rc = wyoming_write_event(conn, &evt);
			wyoming_event_free(&evt);
			if (rc != WYOMING_OK)
				return rc;

			offset += send_bytes;
		}
	}

	/* 4. Send audio-stop */
	{
		wyoming_event_t evt;
		wyoming_event_init(&evt);
		evt.type = strdup(WYOMING_EVENT_AUDIO_STOP);

		rc = wyoming_write_event(conn, &evt);
		wyoming_event_free(&evt);
		if (rc != WYOMING_OK)
			return rc;
	}

	/* 5. Read transcript response */
	for (;;) {
		wyoming_event_t event;
		wyoming_event_init(&event);

		rc = wyoming_read_event(conn, &event);
		if (rc != WYOMING_OK)
			return rc;

		if (strcmp(event.type, WYOMING_EVENT_TRANSCRIPT) == 0) {
			if (event.data) {
				cJSON *t = cJSON_GetObjectItemCaseSensitive(event.data,
				                                            "text");
				if (cJSON_IsString(t) && t->valuestring)
					*text_out = strdup(t->valuestring);
			}
			wyoming_event_free(&event);
			return *text_out ? WYOMING_OK : WYOMING_ERR_PROTO;
		}

		if (strcmp(event.type, WYOMING_EVENT_ERROR) == 0) {
			wyoming_event_free(&event);
			return WYOMING_ERR_PROTO;
		}

		/* Skip unknown events */
		wyoming_event_free(&event);
	}
}

/* ── Close ───────────────────────────────────────────────────── */

void wyoming_close(wyoming_conn_t *conn)
{
	if (!conn) return;
	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
}
