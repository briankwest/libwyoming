#include "config.h"

#ifdef WYOMING_HAVE_PIPER

#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Piper C API declarations (from piper.h) */
typedef struct piper_synthesizer piper_synthesizer;

typedef struct {
	float  *samples;
	size_t  num_samples;
	int     channels;
	int     sample_rate;
	int     is_last;
} piper_audio_chunk;

typedef struct {
	int   speaker_id;
	float length_scale;
	float noise_scale;
	float noise_w;
} piper_synthesize_options;

#define PIPER_OK   0
#define PIPER_DONE 1

extern piper_synthesizer      *piper_create(const char *model_path,
                                            const char *config_path,
                                            const char *espeak_data_path);
extern piper_synthesize_options piper_default_synthesize_options(
                                            piper_synthesizer *synth);
extern int                      piper_synthesize_start(
                                            piper_synthesizer *synth,
                                            const char *text,
                                            piper_synthesize_options *opts);
extern int                      piper_synthesize_next(
                                            piper_synthesizer *synth,
                                            piper_audio_chunk *chunk);
extern void                     piper_destroy(piper_synthesizer *synth);

/* ── Piper engine structure ──────────────────────────────────── */

struct wyoming_piper {
	piper_synthesizer *synth;

	int    sample_rate;
	char  *voice_name;
	char  *language;

	char **speaker_names;
	int   *speaker_ids;
	int    num_speakers;
};

/* ── Helper: extract filename stem ───────────────────────────── */

static char *extract_stem(const char *path)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	const char *dot = strstr(base, ".onnx");
	size_t len = dot ? (size_t)(dot - base) : strlen(base);

	char *stem = malloc(len + 1);
	if (stem) {
		memcpy(stem, base, len);
		stem[len] = '\0';
	}
	return stem;
}

/* ── Helper: read file into buffer ───────────────────────────── */

static char *read_file(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (sz <= 0) { fclose(f); return NULL; }

	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }

	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);

	buf[rd] = '\0';
	if (len_out) *len_out = rd;
	return buf;
}

/* ── Create ──────────────────────────────────────────────────── */

wyoming_piper_t *wyoming_piper_create(const char *model_path,
                                      const char *espeak_data_path)
{
	if (!model_path || !espeak_data_path)
		return NULL;

	wyoming_piper_t *p = calloc(1, sizeof(*p));
	if (!p) return NULL;

	/* Derive config path: model.onnx -> model.onnx.json */
	size_t mlen = strlen(model_path);
	char *config_path = malloc(mlen + 6);
	if (!config_path) { free(p); return NULL; }
	snprintf(config_path, mlen + 6, "%s.json", model_path);

	/* Parse .onnx.json config */
	char *json_str = read_file(config_path, NULL);
	if (!json_str) {
		free(config_path);
		free(p);
		return NULL;
	}

	cJSON *root = cJSON_Parse(json_str);
	free(json_str);
	if (!root) {
		free(config_path);
		free(p);
		return NULL;
	}

	/* Extract audio.sample_rate */
	cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
	if (audio) {
		cJSON *sr = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
		if (cJSON_IsNumber(sr))
			p->sample_rate = (int)sr->valuedouble;
	}
	if (p->sample_rate <= 0)
		p->sample_rate = 22050;

	/* Extract language.code */
	cJSON *lang = cJSON_GetObjectItemCaseSensitive(root, "language");
	if (lang) {
		cJSON *code = cJSON_GetObjectItemCaseSensitive(lang, "code");
		if (cJSON_IsString(code))
			p->language = strdup(code->valuestring);
	}

	/* Extract num_speakers */
	cJSON *ns = cJSON_GetObjectItemCaseSensitive(root, "num_speakers");
	if (cJSON_IsNumber(ns))
		p->num_speakers = (int)ns->valuedouble;

	/* Extract speaker_id_map */
	cJSON *sid_map = cJSON_GetObjectItemCaseSensitive(root,
	                                                   "speaker_id_map");
	if (cJSON_IsObject(sid_map)) {
		int count = cJSON_GetArraySize(sid_map);
		if (count > 0) {
			p->speaker_names = calloc((size_t)count, sizeof(char *));
			p->speaker_ids = calloc((size_t)count, sizeof(int));
			if (p->speaker_names && p->speaker_ids) {
				int idx = 0;
				cJSON *item;
				cJSON_ArrayForEach(item, sid_map) {
					p->speaker_names[idx] = strdup(item->string);
					p->speaker_ids[idx] = (int)item->valuedouble;
					idx++;
				}
				p->num_speakers = count;
			}
		}
	}

	cJSON_Delete(root);

	/* Derive voice name from model filename */
	p->voice_name = extract_stem(model_path);

	/* Initialize Piper synthesizer */
	p->synth = piper_create(model_path, config_path, espeak_data_path);
	free(config_path);

	if (!p->synth) {
		wyoming_piper_destroy(p);
		return NULL;
	}

	return p;
}

/* ── TTS callback ────────────────────────────────────────────── */

static wyoming_error_t piper_tts_callback(
	const char *text,
	const char *voice,
	const char *speaker,
	int16_t **pcm_out,
	size_t *samples_out,
	wyoming_audio_format_t *format_out,
	void *userdata)
{
	(void)voice; /* single model loaded */

	wyoming_piper_t *p = (wyoming_piper_t *)userdata;
	if (!p || !p->synth || !text)
		return WYOMING_ERR_INVAL;

	piper_synthesize_options opts = piper_default_synthesize_options(p->synth);

	/* Resolve speaker name to ID */
	if (speaker && p->num_speakers > 1) {
		int sid = wyoming_piper_speaker_id(p, speaker);
		if (sid >= 0) opts.speaker_id = sid;
	}

	int rc = piper_synthesize_start(p->synth, text, &opts);
	if (rc != PIPER_OK)
		return WYOMING_ERR_PIPER;

	/* Collect chunks into growing buffer */
	size_t capacity = (size_t)p->sample_rate * 2;
	size_t total = 0;
	int16_t *pcm = malloc(capacity * sizeof(int16_t));
	if (!pcm) return WYOMING_ERR_NOMEM;

	piper_audio_chunk chunk;
	while ((rc = piper_synthesize_next(p->synth, &chunk)) == PIPER_OK) {
		if (total + chunk.num_samples > capacity) {
			capacity = (total + chunk.num_samples) * 2;
			int16_t *tmp = realloc(pcm, capacity * sizeof(int16_t));
			if (!tmp) { free(pcm); return WYOMING_ERR_NOMEM; }
			pcm = tmp;
		}

		/* Convert float samples to int16 */
		for (size_t i = 0; i < chunk.num_samples; i++) {
			float s = chunk.samples[i];
			if (s > 1.0f) s = 1.0f;
			if (s < -1.0f) s = -1.0f;
			pcm[total + i] = (int16_t)(s * 32767.0f);
		}
		total += chunk.num_samples;

		if (chunk.is_last) break;
	}

	*pcm_out = pcm;
	*samples_out = total;
	format_out->rate = p->sample_rate;
	format_out->width = 2;
	format_out->channels = 1;

	return WYOMING_OK;
}

/* ── Public API ──────────────────────────────────────────────── */

wyoming_tts_fn wyoming_piper_get_callback(void)
{
	return piper_tts_callback;
}

void *wyoming_piper_as_userdata(wyoming_piper_t *piper)
{
	return (void *)piper;
}

int wyoming_piper_sample_rate(const wyoming_piper_t *piper)
{
	return piper ? piper->sample_rate : 0;
}

const char *wyoming_piper_voice_name(const wyoming_piper_t *piper)
{
	return piper ? piper->voice_name : NULL;
}

const char *wyoming_piper_language(const wyoming_piper_t *piper)
{
	return piper ? piper->language : NULL;
}

int wyoming_piper_num_speakers(const wyoming_piper_t *piper)
{
	return piper ? piper->num_speakers : 0;
}

const char *wyoming_piper_speaker_name(const wyoming_piper_t *piper,
                                       int index)
{
	if (!piper || index < 0 || index >= piper->num_speakers)
		return NULL;
	return piper->speaker_names ? piper->speaker_names[index] : NULL;
}

int wyoming_piper_speaker_id(const wyoming_piper_t *piper,
                             const char *name)
{
	if (!piper || !name || !piper->speaker_names)
		return -1;
	for (int i = 0; i < piper->num_speakers; i++) {
		if (piper->speaker_names[i] &&
		    strcmp(piper->speaker_names[i], name) == 0)
			return piper->speaker_ids[i];
	}
	return -1;
}

void wyoming_piper_destroy(wyoming_piper_t *piper)
{
	if (!piper) return;

	if (piper->synth)
		piper_destroy(piper->synth);

	free(piper->voice_name);
	free(piper->language);

	if (piper->speaker_names) {
		for (int i = 0; i < piper->num_speakers; i++)
			free(piper->speaker_names[i]);
		free(piper->speaker_names);
	}
	free(piper->speaker_ids);

	free(piper);
}

#endif /* WYOMING_HAVE_PIPER */
