#ifndef LIBWYOMING_WYOMING_H
#define LIBWYOMING_WYOMING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────── */
#include "version.h"

/* ── Error codes ─────────────────────────────────────────────── */
typedef enum {
	WYOMING_OK          =  0,
	WYOMING_ERR_IO      = -1,
	WYOMING_ERR_PARSE   = -2,
	WYOMING_ERR_NOMEM   = -3,
	WYOMING_ERR_PROTO   = -4,
	WYOMING_ERR_EOF     = -5,
	WYOMING_ERR_TIMEOUT = -6,
	WYOMING_ERR_PIPER   = -7,
	WYOMING_ERR_INVAL   = -8,
} wyoming_error_t;

/* ── Well-known event type strings ───────────────────────────── */
#define WYOMING_EVENT_SYNTHESIZE    "synthesize"
#define WYOMING_EVENT_AUDIO_START   "audio-start"
#define WYOMING_EVENT_AUDIO_CHUNK   "audio-chunk"
#define WYOMING_EVENT_AUDIO_STOP    "audio-stop"
#define WYOMING_EVENT_DESCRIBE      "describe"
#define WYOMING_EVENT_INFO          "info"
#define WYOMING_EVENT_ERROR         "error"

/* ── Forward declaration for cJSON ───────────────────────────── */
struct cJSON;

/* ── Event (protocol unit) ───────────────────────────────────── */
typedef struct {
	char         *type;         /* event type string (heap-allocated) */
	struct cJSON *data;         /* parsed JSON data object, or NULL */
	uint8_t      *payload;      /* binary payload, or NULL */
	size_t        payload_len;  /* payload byte count */
} wyoming_event_t;

void            wyoming_event_init(wyoming_event_t *event);
void            wyoming_event_free(wyoming_event_t *event);

/* ── Audio format ────────────────────────────────────────────── */
typedef struct {
	int rate;       /* sample rate in Hz (e.g., 22050) */
	int width;      /* bytes per sample (2 = int16) */
	int channels;   /* 1 = mono */
} wyoming_audio_format_t;

/* ── Voice / Info structures (for describe responses) ────────── */
typedef struct {
	char  *name;
} wyoming_speaker_t;

typedef struct {
	char               *name;
	char              **languages;
	int                 language_count;
	char               *description;
	wyoming_speaker_t  *speakers;
	int                 speaker_count;
} wyoming_voice_t;

typedef struct {
	char               *name;
	char               *version;
	wyoming_voice_t    *voices;
	int                 voice_count;
} wyoming_tts_info_t;

typedef struct {
	wyoming_tts_info_t *tts;
	int                 tts_count;
} wyoming_info_t;

void wyoming_info_free(wyoming_info_t *info);

/* ── Client API ──────────────────────────────────────────────── */
typedef struct wyoming_conn wyoming_conn_t;

wyoming_conn_t *wyoming_connect(const char *host, uint16_t port);

wyoming_error_t wyoming_read_event(wyoming_conn_t *conn,
                                   wyoming_event_t *event);

wyoming_error_t wyoming_write_event(wyoming_conn_t *conn,
                                    const wyoming_event_t *event);

wyoming_error_t wyoming_synthesize(wyoming_conn_t *conn,
                                   const char *text,
                                   const char *voice);

wyoming_error_t wyoming_synthesize_pcm(wyoming_conn_t *conn,
                                       const char *text,
                                       const char *voice,
                                       int16_t **pcm_out,
                                       size_t *samples_out,
                                       wyoming_audio_format_t *format_out);

wyoming_error_t wyoming_describe(wyoming_conn_t *conn,
                                 wyoming_info_t *info_out);

void wyoming_close(wyoming_conn_t *conn);

/* ── Server API ──────────────────────────────────────────────── */
typedef struct wyoming_server wyoming_server_t;

typedef wyoming_error_t (*wyoming_tts_fn)(
	const char *text,
	const char *voice,
	const char *speaker,
	int16_t **pcm_out,
	size_t *samples_out,
	wyoming_audio_format_t *format_out,
	void *userdata);

wyoming_server_t *wyoming_server_create(const char *bind_addr,
                                        uint16_t port);

void wyoming_server_set_tts(wyoming_server_t *srv,
                            wyoming_tts_fn fn,
                            void *userdata,
                            const char *name,
                            const char *version);

wyoming_error_t wyoming_server_add_voice(wyoming_server_t *srv,
                                         const char *name,
                                         const char *const *languages,
                                         const char *description);

wyoming_error_t wyoming_server_add_speaker(wyoming_server_t *srv,
                                           const char *speaker_name);

wyoming_error_t wyoming_server_run(wyoming_server_t *srv);

void wyoming_server_stop(wyoming_server_t *srv);

uint16_t wyoming_server_port(wyoming_server_t *srv);

void wyoming_server_destroy(wyoming_server_t *srv);

/* ── Piper Engine (optional) ─────────────────────────────────── */
#ifdef WYOMING_HAVE_PIPER

typedef struct wyoming_piper wyoming_piper_t;

wyoming_piper_t *wyoming_piper_create(const char *model_path,
                                      const char *espeak_data_path);

wyoming_tts_fn   wyoming_piper_get_callback(void);

void            *wyoming_piper_as_userdata(wyoming_piper_t *piper);

int              wyoming_piper_sample_rate(const wyoming_piper_t *piper);
const char      *wyoming_piper_voice_name(const wyoming_piper_t *piper);
const char      *wyoming_piper_language(const wyoming_piper_t *piper);
int              wyoming_piper_num_speakers(const wyoming_piper_t *piper);
const char      *wyoming_piper_speaker_name(const wyoming_piper_t *piper,
                                            int index);
int              wyoming_piper_speaker_id(const wyoming_piper_t *piper,
                                         const char *name);

void             wyoming_piper_destroy(wyoming_piper_t *piper);

#endif /* WYOMING_HAVE_PIPER */

#ifdef __cplusplus
}
#endif

#endif /* LIBWYOMING_WYOMING_H */
