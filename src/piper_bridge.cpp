/*
 * piper_bridge.cpp — Minimal Piper TTS engine using libpiper_phonemize + onnxruntime
 *
 * Direct ONNX inference without pulling in the full piper.cpp and its
 * dependencies (spdlog, wavfile, utf8cpp). Uses only:
 *   - piper-phonemize: text → phoneme IDs
 *   - onnxruntime C++ API: run VITS ONNX model → raw PCM
 *
 * Provides the C API expected by libwyoming's piper.c.
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <mutex>
#include <random>

#include <espeak-ng/speak_lib.h>

#include <piper-phonemize/phonemize.hpp>
#include <piper-phonemize/phoneme_ids.hpp>
#include <piper-phonemize/shared.hpp>

#include <onnxruntime_cxx_api.h>

/* Simple JSON parser for model config — just need sample_rate, phoneme_id_map, etc. */
#include "cJSON.h"

extern "C" {

/* ── C API types ───────────────────────────────────────────── */

typedef struct piper_synthesizer {
	Ort::Env           env;
	Ort::Session      *session;
	Ort::SessionOptions session_opts;

	piper::eSpeakPhonemeConfig espeak_config;
	piper::PhonemeIdConfig     phoneme_id_config;

	int    sample_rate;
	int    num_speakers;
	bool   ready;

	std::mutex            mutex;
	std::vector<int16_t>  audio_buf;

	char  *voice_name;
	char  *language;

	piper_synthesizer() : env(ORT_LOGGING_LEVEL_WARNING, "piper"), session(nullptr),
	                       sample_rate(22050), num_speakers(0), ready(false),
	                       voice_name(nullptr), language(nullptr) {}
} piper_synthesizer;

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

/* ── Config parser (reads .onnx.json sidecar) ─────────────── */

static bool load_model_config(const char *json_path, piper_synthesizer *synth)
{
	FILE *f = fopen(json_path, "r");
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = (char *)malloc(sz + 1);
	if (!buf) { fclose(f); return false; }
	fread(buf, 1, sz, f);
	buf[sz] = '\0';
	fclose(f);

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) return false;

	/* sample_rate */
	cJSON *audio = cJSON_GetObjectItem(root, "audio");
	if (audio) {
		cJSON *sr = cJSON_GetObjectItem(audio, "sample_rate");
		if (cJSON_IsNumber(sr)) synth->sample_rate = (int)sr->valuedouble;
	}

	/* num_speakers */
	cJSON *ns = cJSON_GetObjectItem(root, "num_speakers");
	if (cJSON_IsNumber(ns)) synth->num_speakers = (int)ns->valuedouble;

	/* espeak voice */
	cJSON *espeak = cJSON_GetObjectItem(root, "espeak");
	if (espeak) {
		cJSON *voice = cJSON_GetObjectItem(espeak, "voice");
		if (cJSON_IsString(voice))
			synth->espeak_config.voice = voice->valuestring;
	}

	/* phoneme_id_map */
	cJSON *pidmap = cJSON_GetObjectItem(root, "phoneme_id_map");
	if (cJSON_IsObject(pidmap)) {
		cJSON *entry = pidmap->child;
		while (entry) {
			if (cJSON_IsArray(entry) && entry->string) {
				/* Key is a UTF-32 codepoint character, value is array of int64 IDs */
				std::string key_str(entry->string);
				/* Convert first char to Phoneme (char32_t) */
				if (!key_str.empty()) {
					/* Simple: take first UTF-8 codepoint */
					char32_t phoneme = 0;
					const char *p = key_str.c_str();
					unsigned char c = (unsigned char)*p;
					if (c < 0x80) phoneme = c;
					else if (c < 0xe0) phoneme = (c & 0x1f) << 6 | (p[1] & 0x3f);
					else if (c < 0xf0) phoneme = (c & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
					else phoneme = (c & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f);

					std::vector<piper::PhonemeId> ids;
					cJSON *id_elem = entry->child;
					while (id_elem) {
						if (cJSON_IsNumber(id_elem))
							ids.push_back((piper::PhonemeId)id_elem->valuedouble);
						id_elem = id_elem->next;
					}
					if (!synth->phoneme_id_config.phonemeIdMap)
						synth->phoneme_id_config.phonemeIdMap = std::make_shared<piper::PhonemeIdMap>();
					(*synth->phoneme_id_config.phonemeIdMap)[phoneme] = ids;
				}
			}
			entry = entry->next;
		}
	}

	/* Set defaults from config */
	cJSON *inference = cJSON_GetObjectItem(root, "inference");
	(void)inference; /* noise/length scales set via options */

	/* Language from espeak voice (e.g., "en-us" → "en_US") */
	if (!synth->espeak_config.voice.empty()) {
		std::string lang = synth->espeak_config.voice;
		/* Convert "en-us" to "en_US" */
		auto dash = lang.find('-');
		if (dash != std::string::npos) {
			for (size_t i = dash + 1; i < lang.size(); i++)
				lang[i] = toupper(lang[i]);
			lang[dash] = '_';
		}
		synth->language = strdup(lang.c_str());
	}

	cJSON_Delete(root);
	return true;
}

/* ── C API implementation ─────────────────────────────────── */

piper_synthesizer *piper_create(const char *model_path,
                                const char *config_path,
                                const char *espeak_data_path)
{
	auto *synth = new (std::nothrow) piper_synthesizer();
	if (!synth) return nullptr;

	try {
		/* Initialize espeak-ng */
		int result = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
		                                espeak_data_path ? espeak_data_path : "/usr/share/espeak-ng-data",
		                                0);
		if (result < 0) {
			fprintf(stderr, "piper: espeak_Initialize failed\n");
			delete synth;
			return nullptr;
		}

		/* Load model config */
		std::string cfg_path;
		if (config_path && config_path[0])
			cfg_path = config_path;
		else
			cfg_path = std::string(model_path) + ".json";

		if (!load_model_config(cfg_path.c_str(), synth)) {
			fprintf(stderr, "piper: failed to load config: %s\n", cfg_path.c_str());
			delete synth;
			return nullptr;
		}

		/* Phoneme ID config uses default pad/bos/eos chars from the struct */
		synth->phoneme_id_config.interspersePad = true;

		/* Load ONNX model */
		synth->session_opts.SetIntraOpNumThreads(2);
		synth->session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

		synth->session = new Ort::Session(synth->env, model_path,
		                                   synth->session_opts);

		/* Extract voice name from path */
		std::string path(model_path);
		auto last_slash = path.rfind('/');
		if (last_slash != std::string::npos) {
			auto dir = path.substr(0, last_slash);
			auto dir_slash = dir.rfind('/');
			if (dir_slash != std::string::npos)
				synth->voice_name = strdup(dir.substr(dir_slash + 1).c_str());
		}

		synth->ready = true;
		return synth;
	} catch (const std::exception &e) {
		fprintf(stderr, "piper_create error: %s\n", e.what());
		delete synth;
		return nullptr;
	}
}

piper_synthesize_options piper_default_synthesize_options(
	piper_synthesizer *synth)
{
	piper_synthesize_options opts = {};
	opts.speaker_id = 0;
	opts.length_scale = 1.0f;
	opts.noise_scale = 0.667f;
	opts.noise_w = 0.8f;
	(void)synth;
	return opts;
}

int piper_synthesize_start(piper_synthesizer *synth,
                           const char *text,
                           piper_synthesize_options *opts)
{
	if (!synth || !synth->ready || !text || !synth->session)
		return -1;

	std::lock_guard<std::mutex> lock(synth->mutex);
	synth->audio_buf.clear();

	try {
		/* 1. Phonemize: text → phoneme strings */
		std::vector<std::vector<piper::Phoneme>> phonemes;
		piper::phonemize_eSpeak(std::string(text), synth->espeak_config, phonemes);

		if (phonemes.empty())
			return -1;

		/* 2. Convert phonemes → IDs */
		std::vector<piper::PhonemeId> all_ids;
		for (auto &sentence : phonemes) {
			std::vector<piper::PhonemeId> ids;
			std::map<piper::Phoneme, std::size_t> missing;
			piper::phonemes_to_ids(sentence, synth->phoneme_id_config, ids, missing);
			all_ids.insert(all_ids.end(), ids.begin(), ids.end());
		}

		if (all_ids.empty())
			return -1;

		/* 3. Run ONNX inference (VITS model) */
		Ort::AllocatorWithDefaultOptions allocator;
		Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
			OrtArenaAllocator, OrtMemTypeDefault);

		/* Input: phoneme IDs [1, seq_len] */
		std::vector<int64_t> input_ids(all_ids.begin(), all_ids.end());
		int64_t input_shape[] = { 1, (int64_t)input_ids.size() };
		Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
			mem_info, input_ids.data(), input_ids.size(),
			input_shape, 2);

		/* Input: input lengths [1] */
		int64_t input_len = (int64_t)input_ids.size();
		int64_t len_shape[] = { 1 };
		Ort::Value len_tensor = Ort::Value::CreateTensor<int64_t>(
			mem_info, &input_len, 1, len_shape, 1);

		/* Input: scales [3] — noise_scale, length_scale, noise_w */
		float scales[3] = {
			opts ? opts->noise_scale : 0.667f,
			opts ? opts->length_scale : 1.0f,
			opts ? opts->noise_w : 0.8f
		};
		int64_t scales_shape[] = { 3 };
		Ort::Value scales_tensor = Ort::Value::CreateTensor<float>(
			mem_info, scales, 3, scales_shape, 1);

		/* Build input array */
		std::vector<Ort::Value> inputs;
		inputs.push_back(std::move(input_tensor));
		inputs.push_back(std::move(len_tensor));
		inputs.push_back(std::move(scales_tensor));

		/* Add speaker ID for multi-speaker models */
		int64_t sid = opts ? opts->speaker_id : 0;
		int64_t sid_shape[] = { 1 };
		if (synth->num_speakers > 1) {
			Ort::Value sid_tensor = Ort::Value::CreateTensor<int64_t>(
				mem_info, &sid, 1, sid_shape, 1);
			inputs.push_back(std::move(sid_tensor));
		}

		/* Input/output names */
		const char *input_names_3[] = { "input", "input_lengths", "scales" };
		const char *input_names_4[] = { "input", "input_lengths", "scales", "sid" };
		const char **input_names = (synth->num_speakers > 1) ? input_names_4 : input_names_3;
		int num_inputs = (synth->num_speakers > 1) ? 4 : 3;
		const char *output_names[] = { "output" };

		auto outputs = synth->session->Run(
			Ort::RunOptions{nullptr},
			input_names, inputs.data(), (size_t)num_inputs,
			output_names, 1);

		/* Extract PCM from output tensor [1, 1, num_samples] */
		auto &out_tensor = outputs[0];
		auto out_shape = out_tensor.GetTensorTypeAndShapeInfo().GetShape();
		size_t num_samples = 1;
		for (auto dim : out_shape) num_samples *= (size_t)dim;

		const float *audio_data = out_tensor.GetTensorData<float>();

		/* Convert float [-1,1] to int16 */
		synth->audio_buf.resize(num_samples);
		for (size_t i = 0; i < num_samples; i++) {
			float s = audio_data[i];
			if (s > 1.0f) s = 1.0f;
			if (s < -1.0f) s = -1.0f;
			synth->audio_buf[i] = (int16_t)(s * 32767.0f);
		}

		return PIPER_OK;
	} catch (const std::exception &e) {
		fprintf(stderr, "piper_synthesize error: %s\n", e.what());
		return -1;
	}
}

int piper_synthesize_next(piper_synthesizer *synth,
                          piper_audio_chunk *chunk)
{
	if (!synth || !chunk) return -1;

	std::lock_guard<std::mutex> lock(synth->mutex);

	if (synth->audio_buf.empty()) {
		chunk->samples = nullptr;
		chunk->num_samples = 0;
		chunk->is_last = 1;
		return PIPER_DONE;
	}

	/* Return float samples (caller expects float*) */
	size_t n = synth->audio_buf.size();
	float *fsamples = (float *)malloc(n * sizeof(float));
	if (!fsamples) return -1;

	for (size_t i = 0; i < n; i++)
		fsamples[i] = (float)synth->audio_buf[i] / 32768.0f;

	chunk->samples = fsamples;
	chunk->num_samples = n;
	chunk->channels = 1;
	chunk->sample_rate = synth->sample_rate;
	chunk->is_last = 1;

	synth->audio_buf.clear();
	return PIPER_OK;
}

void piper_destroy(piper_synthesizer *synth)
{
	if (!synth) return;
	delete synth->session;
	free(synth->voice_name);
	free(synth->language);
	delete synth;
}

} /* extern "C" */
