/*
 * sherpa.c — Sherpa-ONNX ASR engine for libwyoming
 *
 * Wraps the sherpa-onnx C API for both offline (batch) and online
 * (streaming) speech recognition.  Provides callbacks that plug
 * directly into the Wyoming server via wyoming_server_set_asr() and
 * wyoming_server_set_asr_streaming().
 *
 * Offline: Whisper, Paraformer, NeMo CTC, SenseVoice
 * Online:  Zipformer transducer, Paraformer, NeMo CTC
 */

#include "config.h"

#ifdef WYOMING_HAVE_SHERPA

#include "internal.h"
#include <sherpa-onnx/c-api/c-api.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Engine state ───────────────────────────────────────────── */

struct wyoming_sherpa {
	int is_streaming;

	/* Offline recognizer (batch ASR) */
	const SherpaOnnxOfflineRecognizer *offline;

	/* Online recognizer (streaming ASR) */
	const SherpaOnnxOnlineRecognizer *online;

	/* Metadata */
	char model_name[128];
	char language[16];
	int  sample_rate;
	int  num_threads;
};

/* Streaming session context */
typedef struct {
	const SherpaOnnxOnlineStream *stream;
	const SherpaOnnxOnlineRecognizer *recognizer;
	int sample_rate;
} sherpa_stream_ctx_t;

/* ── Helper: convert int16 to float ─────────────────────────── */

static float *int16_to_float(const int16_t *pcm, size_t samples)
{
	float *f = malloc(samples * sizeof(float));
	if (!f) return NULL;
	for (size_t i = 0; i < samples; i++)
		f[i] = (float)pcm[i] / 32768.0f;
	return f;
}

/* ── Offline (batch) ASR ────────────────────────────────────── */

wyoming_sherpa_t *wyoming_sherpa_create(const char *model_dir,
                                        const char *model_type,
                                        const char *language)
{
	if (!model_dir)
		return NULL;

	wyoming_sherpa_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;

	s->is_streaming = 0;
	s->sample_rate = 16000;
	s->num_threads = 4;

	if (language)
		snprintf(s->language, sizeof(s->language), "%s", language);

	SherpaOnnxOfflineRecognizerConfig config;
	memset(&config, 0, sizeof(config));
	config.feat_config.sample_rate = 16000;
	config.feat_config.feature_dim = 80;
	config.decoding_method = "greedy_search";
	config.model_config.num_threads = s->num_threads;
	config.model_config.provider = "cpu";
	config.model_config.debug = 0;

	/* All path buffers must outlive the config struct (used by Create below) */
	char enc_path[512], dec_path[512], model_path[512], tokens_path[512];

	snprintf(tokens_path, sizeof(tokens_path), "%s/tokens.txt", model_dir);
	config.model_config.tokens = tokens_path;

	if (!model_type || strcmp(model_type, "whisper") == 0) {
		snprintf(enc_path, sizeof(enc_path), "%s/encoder.onnx", model_dir);
		snprintf(dec_path, sizeof(dec_path), "%s/decoder.onnx", model_dir);
		config.model_config.whisper.encoder = enc_path;
		config.model_config.whisper.decoder = dec_path;
		if (language && language[0])
			config.model_config.whisper.language = language;
		config.model_config.whisper.tail_paddings = 800;
		snprintf(s->model_name, sizeof(s->model_name), "whisper");
	} else if (strcmp(model_type, "paraformer") == 0) {
		snprintf(model_path, sizeof(model_path), "%s/model.onnx", model_dir);
		config.model_config.paraformer.model = model_path;
		snprintf(s->model_name, sizeof(s->model_name), "paraformer");
	} else if (strcmp(model_type, "nemo_ctc") == 0) {
		snprintf(model_path, sizeof(model_path), "%s/model.onnx", model_dir);
		config.model_config.nemo_ctc.model = model_path;
		snprintf(s->model_name, sizeof(s->model_name), "nemo_ctc");
	} else if (strcmp(model_type, "sense_voice") == 0) {
		snprintf(model_path, sizeof(model_path), "%s/model.onnx", model_dir);
		config.model_config.sense_voice.model = model_path;
		if (language && language[0])
			config.model_config.sense_voice.language = language;
		snprintf(s->model_name, sizeof(s->model_name), "sense_voice");
	} else {
		free(s);
		return NULL;
	}

	s->offline = SherpaOnnxCreateOfflineRecognizer(&config);
	if (!s->offline) {
		free(s);
		return NULL;
	}

	return s;
}

/* Batch ASR via offline recognizer */
static wyoming_error_t sherpa_batch_offline(wyoming_sherpa_t *s,
                                             const float *fsamples,
                                             size_t samples, int rate,
                                             char **text_out)
{
	const SherpaOnnxOfflineStream *stream =
		SherpaOnnxCreateOfflineStream(s->offline);
	if (!stream) return WYOMING_ERR_PROTO;

	SherpaOnnxAcceptWaveformOffline(stream, rate, fsamples, (int32_t)samples);
	SherpaOnnxDecodeOfflineStream(s->offline, stream);

	const SherpaOnnxOfflineRecognizerResult *result =
		SherpaOnnxGetOfflineStreamResult(stream);

	if (result && result->text && result->text[0])
		*text_out = strdup(result->text);

	if (result)
		SherpaOnnxDestroyOfflineRecognizerResult(result);
	SherpaOnnxDestroyOfflineStream(stream);

	return *text_out ? WYOMING_OK : WYOMING_ERR_PROTO;
}

/* Batch ASR via online (streaming) recognizer — feed all audio at once */
static wyoming_error_t sherpa_batch_via_online(wyoming_sherpa_t *s,
                                                const float *fsamples,
                                                size_t samples, int rate,
                                                char **text_out)
{
	const SherpaOnnxOnlineStream *stream =
		SherpaOnnxCreateOnlineStream(s->online);
	if (!stream) return WYOMING_ERR_PROTO;

	SherpaOnnxOnlineStreamAcceptWaveform(stream, rate, fsamples,
	                                      (int32_t)samples);
	SherpaOnnxOnlineStreamInputFinished(stream);

	while (SherpaOnnxIsOnlineStreamReady(s->online, stream))
		SherpaOnnxDecodeOnlineStream(s->online, stream);

	const SherpaOnnxOnlineRecognizerResult *result =
		SherpaOnnxGetOnlineStreamResult(s->online, stream);

	if (result && result->text && result->text[0])
		*text_out = strdup(result->text);

	if (result)
		SherpaOnnxDestroyOnlineRecognizerResult(result);
	SherpaOnnxDestroyOnlineStream(stream);

	return *text_out ? WYOMING_OK : WYOMING_ERR_PROTO;
}

/* Batch ASR callback — works with both offline and online recognizers */
static wyoming_error_t sherpa_batch_asr(const int16_t *pcm, size_t samples,
                                         const wyoming_audio_format_t *format,
                                         const char *language,
                                         char **text_out, void *userdata)
{
	(void)language;
	wyoming_sherpa_t *s = userdata;
	if (!s || !pcm || samples == 0 || !text_out)
		return WYOMING_ERR_INVAL;
	if (!s->offline && !s->online)
		return WYOMING_ERR_INVAL;

	*text_out = NULL;

	float *fsamples = int16_to_float(pcm, samples);
	if (!fsamples)
		return WYOMING_ERR_NOMEM;

	wyoming_error_t rc;
	if (s->offline)
		rc = sherpa_batch_offline(s, fsamples, samples, format->rate, text_out);
	else
		rc = sherpa_batch_via_online(s, fsamples, samples, format->rate, text_out);

	free(fsamples);
	return rc;
}

/* ── Online (streaming) ASR ─────────────────────────────────── */

wyoming_sherpa_t *wyoming_sherpa_create_streaming(const char *model_dir,
                                                    const char *model_type,
                                                    const char *language)
{
	if (!model_dir)
		return NULL;

	wyoming_sherpa_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;

	s->is_streaming = 1;
	s->sample_rate = 16000;
	s->num_threads = 4;

	if (language)
		snprintf(s->language, sizeof(s->language), "%s", language);

	SherpaOnnxOnlineRecognizerConfig config;
	memset(&config, 0, sizeof(config));
	config.feat_config.sample_rate = 16000;
	config.feat_config.feature_dim = 80;
	config.decoding_method = "greedy_search";
	config.model_config.num_threads = s->num_threads;
	config.model_config.provider = "cpu";
	config.model_config.debug = 0;
	config.enable_endpoint = 1;
	config.rule1_min_trailing_silence = 2.4f;
	config.rule2_min_trailing_silence = 1.2f;
	config.rule3_min_utterance_length = 20.0f;

	char enc_path[512], dec_path[512], joiner_path[512],
	     model_path[512], tokens_path[512];

	snprintf(tokens_path, sizeof(tokens_path), "%s/tokens.txt", model_dir);
	config.model_config.tokens = tokens_path;

	if (!model_type || strcmp(model_type, "zipformer") == 0) {
		snprintf(enc_path, sizeof(enc_path), "%s/encoder.onnx", model_dir);
		snprintf(dec_path, sizeof(dec_path), "%s/decoder.onnx", model_dir);
		snprintf(joiner_path, sizeof(joiner_path), "%s/joiner.onnx", model_dir);
		config.model_config.transducer.encoder = enc_path;
		config.model_config.transducer.decoder = dec_path;
		config.model_config.transducer.joiner = joiner_path;
		snprintf(s->model_name, sizeof(s->model_name), "zipformer");
	} else if (strcmp(model_type, "paraformer") == 0) {
		snprintf(enc_path, sizeof(enc_path), "%s/encoder.onnx", model_dir);
		snprintf(dec_path, sizeof(dec_path), "%s/decoder.onnx", model_dir);
		config.model_config.paraformer.encoder = enc_path;
		config.model_config.paraformer.decoder = dec_path;
		snprintf(s->model_name, sizeof(s->model_name), "paraformer");
	} else if (strcmp(model_type, "nemo_ctc") == 0) {
		snprintf(model_path, sizeof(model_path), "%s/model.onnx", model_dir);
		config.model_config.nemo_ctc.model = model_path;
		snprintf(s->model_name, sizeof(s->model_name), "nemo_ctc");
	} else {
		free(s);
		return NULL;
	}

	s->online = SherpaOnnxCreateOnlineRecognizer(&config);
	if (!s->online) {
		free(s);
		return NULL;
	}

	return s;
}

/* Streaming callbacks */

static void *sherpa_stream_create(const wyoming_audio_format_t *format,
                                   const char *language,
                                   void *userdata)
{
	(void)language;
	wyoming_sherpa_t *s = userdata;
	if (!s || !s->online)
		return NULL;

	const SherpaOnnxOnlineStream *stream =
		SherpaOnnxCreateOnlineStream(s->online);
	if (!stream)
		return NULL;

	sherpa_stream_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SherpaOnnxDestroyOnlineStream(stream);
		return NULL;
	}

	ctx->stream = stream;
	ctx->recognizer = s->online;
	ctx->sample_rate = format ? format->rate : 16000;

	return ctx;
}

static wyoming_error_t sherpa_stream_process(void *stream_ctx,
                                              const int16_t *pcm,
                                              size_t samples,
                                              const wyoming_audio_format_t *format,
                                              int is_final,
                                              char **text_out,
                                              void *userdata)
{
	(void)format;
	(void)userdata;
	sherpa_stream_ctx_t *ctx = stream_ctx;
	if (!ctx || !ctx->stream || !ctx->recognizer)
		return WYOMING_ERR_INVAL;

	if (text_out)
		*text_out = NULL;

	if (pcm && samples > 0) {
		/* Convert int16 to float and feed to stream */
		float *fsamples = int16_to_float(pcm, samples);
		if (!fsamples)
			return WYOMING_ERR_NOMEM;

		SherpaOnnxOnlineStreamAcceptWaveform(ctx->stream,
		                                      ctx->sample_rate,
		                                      fsamples,
		                                      (int32_t)samples);
		free(fsamples);

		/* Decode if ready */
		while (SherpaOnnxIsOnlineStreamReady(ctx->recognizer, ctx->stream))
			SherpaOnnxDecodeOnlineStream(ctx->recognizer, ctx->stream);

		/* Get partial result */
		if (text_out) {
			const SherpaOnnxOnlineRecognizerResult *r =
				SherpaOnnxGetOnlineStreamResult(ctx->recognizer, ctx->stream);
			if (r && r->text && r->text[0])
				*text_out = strdup(r->text);
			if (r)
				SherpaOnnxDestroyOnlineRecognizerResult(r);
		}
	}

	if (is_final && text_out) {
		/* Input stream finished — flush any remaining audio */
		SherpaOnnxOnlineStreamInputFinished(ctx->stream);
		while (SherpaOnnxIsOnlineStreamReady(ctx->recognizer, ctx->stream))
			SherpaOnnxDecodeOnlineStream(ctx->recognizer, ctx->stream);

		const SherpaOnnxOnlineRecognizerResult *r =
			SherpaOnnxGetOnlineStreamResult(ctx->recognizer, ctx->stream);
		if (r && r->text && r->text[0]) {
			free(*text_out);
			*text_out = strdup(r->text);
		}
		if (r)
			SherpaOnnxDestroyOnlineRecognizerResult(r);
	}

	return WYOMING_OK;
}

static void sherpa_stream_destroy(void *stream_ctx, void *userdata)
{
	(void)userdata;
	sherpa_stream_ctx_t *ctx = stream_ctx;
	if (!ctx) return;
	if (ctx->stream)
		SherpaOnnxDestroyOnlineStream(ctx->stream);
	free(ctx);
}

/* ── Public API ─────────────────────────────────────────────── */

wyoming_asr_fn wyoming_sherpa_get_asr_callback(void)
{
	return sherpa_batch_asr;
}

wyoming_asr_stream_create_fn wyoming_sherpa_get_stream_create(void)
{
	return sherpa_stream_create;
}

wyoming_asr_stream_fn wyoming_sherpa_get_stream_process(void)
{
	return sherpa_stream_process;
}

wyoming_asr_stream_destroy_fn wyoming_sherpa_get_stream_destroy(void)
{
	return sherpa_stream_destroy;
}

void *wyoming_sherpa_as_userdata(wyoming_sherpa_t *s)
{
	return s;
}

const char *wyoming_sherpa_model_name(const wyoming_sherpa_t *s)
{
	return s ? s->model_name : NULL;
}

const char *wyoming_sherpa_language(const wyoming_sherpa_t *s)
{
	return s ? s->language : NULL;
}

int wyoming_sherpa_sample_rate(const wyoming_sherpa_t *s)
{
	return s ? s->sample_rate : 0;
}

int wyoming_sherpa_is_streaming(const wyoming_sherpa_t *s)
{
	return s ? s->is_streaming : 0;
}

void wyoming_sherpa_destroy(wyoming_sherpa_t *s)
{
	if (!s) return;
	if (s->offline)
		SherpaOnnxDestroyOfflineRecognizer(s->offline);
	if (s->online)
		SherpaOnnxDestroyOnlineRecognizer(s->online);
	free(s);
}

#endif /* WYOMING_HAVE_SHERPA */
