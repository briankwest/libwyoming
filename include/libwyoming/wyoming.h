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
#define WYOMING_EVENT_TRANSCRIBE    "transcribe"
#define WYOMING_EVENT_TRANSCRIPT    "transcript"
#define WYOMING_EVENT_TRANSCRIBE_START "transcribe-start"
#define WYOMING_EVENT_TRANSCRIBE_STOP  "transcribe-stop"
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
	char               *name;
	char              **languages;
	int                 language_count;
	char               *description;
} wyoming_model_t;

typedef struct {
	char               *name;
	char               *version;
	wyoming_model_t    *models;
	int                 model_count;
} wyoming_asr_info_t;

typedef struct {
	wyoming_tts_info_t *tts;
	int                 tts_count;
	wyoming_asr_info_t *asr;
	int                 asr_count;
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

/* Send PCM audio for transcription.  Returns heap-allocated text in
 * *text_out (caller must free).  language may be NULL for auto-detect. */
wyoming_error_t wyoming_transcribe_pcm(wyoming_conn_t *conn,
                                       const int16_t *pcm,
                                       size_t samples,
                                       const wyoming_audio_format_t *format,
                                       const char *language,
                                       char **text_out);

/* Streaming transcription (client side).
 * Call start, then feed chunks, then stop to get final text.
 * Partial transcripts may arrive as events between chunks. */
wyoming_error_t wyoming_transcribe_start(wyoming_conn_t *conn,
                                          const wyoming_audio_format_t *format,
                                          const char *language);

wyoming_error_t wyoming_transcribe_chunk(wyoming_conn_t *conn,
                                          const int16_t *pcm,
                                          size_t samples,
                                          const wyoming_audio_format_t *format);

wyoming_error_t wyoming_transcribe_stop(wyoming_conn_t *conn,
                                         char **text_out);

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

/* Non-streaming (batch) ASR callback.
 * Receives complete audio, returns heap-allocated transcript text. */
typedef wyoming_error_t (*wyoming_asr_fn)(
	const int16_t *pcm,
	size_t samples,
	const wyoming_audio_format_t *format,
	const char *language,          /* NULL = auto-detect */
	char **text_out,               /* heap-allocated, caller frees */
	void *userdata);

void wyoming_server_set_asr(wyoming_server_t *srv,
                            wyoming_asr_fn fn,
                            void *userdata,
                            const char *name,
                            const char *version);

wyoming_error_t wyoming_server_add_asr_model(wyoming_server_t *srv,
                                              const char *name,
                                              const char *const *languages,
                                              const char *description);

/* Streaming ASR callbacks.
 * create:  called on transcribe-start, returns opaque stream context.
 * process: called per audio-chunk (pcm!=NULL) and on audio-stop (pcm=NULL,
 *          is_final=1).  May set *text_out to partial or final transcript.
 * destroy: called after the stream ends. */
typedef void *(*wyoming_asr_stream_create_fn)(
	const wyoming_audio_format_t *format,
	const char *language,
	void *userdata);

typedef wyoming_error_t (*wyoming_asr_stream_fn)(
	void *stream_ctx,
	const int16_t *pcm,            /* NULL on final call */
	size_t samples,                /* 0 on final call */
	const wyoming_audio_format_t *format,
	int is_final,
	char **text_out,               /* partial/final text, NULL if nothing yet */
	void *userdata);

typedef void (*wyoming_asr_stream_destroy_fn)(
	void *stream_ctx,
	void *userdata);

void wyoming_server_set_asr_streaming(wyoming_server_t *srv,
                                       wyoming_asr_stream_create_fn create_fn,
                                       wyoming_asr_stream_fn process_fn,
                                       wyoming_asr_stream_destroy_fn destroy_fn,
                                       void *userdata);

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

/* ── Sherpa-ONNX ASR Engine (optional) ──────────────────────── */
#ifdef WYOMING_HAVE_SHERPA

typedef struct wyoming_sherpa wyoming_sherpa_t;

/* Create offline (batch) recognizer.
 * model_type: "whisper", "paraformer", "nemo_ctc", "sense_voice" */
wyoming_sherpa_t *wyoming_sherpa_create(const char *model_dir,
                                        const char *model_type,
                                        const char *language);

/* Create online (streaming) recognizer.
 * model_type: "zipformer", "paraformer", "nemo_ctc" */
wyoming_sherpa_t *wyoming_sherpa_create_streaming(const char *model_dir,
                                                    const char *model_type,
                                                    const char *language);

/* Get callbacks for server registration */
wyoming_asr_fn                  wyoming_sherpa_get_asr_callback(void);
wyoming_asr_stream_create_fn    wyoming_sherpa_get_stream_create(void);
wyoming_asr_stream_fn           wyoming_sherpa_get_stream_process(void);
wyoming_asr_stream_destroy_fn   wyoming_sherpa_get_stream_destroy(void);

void *wyoming_sherpa_as_userdata(wyoming_sherpa_t *s);

/* Query */
const char *wyoming_sherpa_model_name(const wyoming_sherpa_t *s);
const char *wyoming_sherpa_language(const wyoming_sherpa_t *s);
int         wyoming_sherpa_sample_rate(const wyoming_sherpa_t *s);
int         wyoming_sherpa_is_streaming(const wyoming_sherpa_t *s);

void wyoming_sherpa_destroy(wyoming_sherpa_t *s);

#endif /* WYOMING_HAVE_SHERPA */

#ifdef __cplusplus
}
#endif

#endif /* LIBWYOMING_WYOMING_H */
