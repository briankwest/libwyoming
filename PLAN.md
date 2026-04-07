# libwyoming Full Scope Plan

## Overview

libwyoming becomes the complete C implementation of the Wyoming voice protocol —
client AND server — with native engine backends (Piper TTS, Sherpa-ONNX ASR),
packaged as Debian packages with systemd services and downloadable model data
packages.

End state: `apt install wyoming-piper-server wyoming-asr-server wyoming-voice-en-us-lessac-high wyoming-model-whisper-base-en`
— fully functional, zero Python, auto-starts on boot.

---

## 1. Library Architecture

### 1.1 Current State (done)

```
libwyoming.so
├── Protocol layer      (event.c)     — read/write Wyoming events over fd
├── Client API          (client.c)    — connect, synthesize_pcm, transcribe_pcm, describe, close
├── Server API          (server.c)    — listen, accept, dispatch TTS callbacks
└── Piper engine        (piper.c)     — load ONNX model via piper_phonemize + onnxruntime
```

### 1.2 Target State

```
libwyoming.so
├── Protocol layer      (event.c)       — unchanged
├── Client API          (client.c)      — unchanged
├── Server API          (server.c)      — EXTENDED: TTS + ASR + streaming ASR dispatch
├── Piper engine        (piper.c)       — unchanged (optional, WYOMING_HAVE_PIPER)
└── Sherpa engine       (sherpa.c)      — NEW (optional, WYOMING_HAVE_SHERPA)

wyoming-piper-server    (bin)           — standalone TTS server binary
wyoming-asr-server      (bin)           — standalone ASR server binary
```

---

## 2. Server API Additions

### 2.1 ASR Callback Types

```c
/* Non-streaming (batch) ASR callback.
 * Receives complete audio, returns complete transcript. */
typedef wyoming_error_t (*wyoming_asr_fn)(
    const int16_t *pcm,
    size_t samples,
    const wyoming_audio_format_t *format,
    const char *language,          /* NULL = auto-detect */
    char **text_out,               /* heap-allocated, caller frees */
    void *userdata);

/* Streaming ASR callback — called per audio chunk.
 * Returns partial/final text as it becomes available.
 * is_final=1 on the last call (after audio-stop). */
typedef wyoming_error_t (*wyoming_asr_stream_fn)(
    const int16_t *pcm,            /* NULL on final call */
    size_t samples,                /* 0 on final call */
    const wyoming_audio_format_t *format,
    int is_final,                  /* 1 = last call, produce final result */
    char **text_out,               /* partial/final text, NULL if nothing yet */
    void *userdata);

/* Streaming ASR session lifecycle */
typedef void *(*wyoming_asr_stream_create_fn)(
    const wyoming_audio_format_t *format,
    const char *language,
    void *userdata);                /* returns opaque stream context */

typedef void (*wyoming_asr_stream_destroy_fn)(
    void *stream_ctx,
    void *userdata);
```

### 2.2 Server Registration API

```c
/* Register non-streaming ASR engine */
void wyoming_server_set_asr(wyoming_server_t *srv,
                            wyoming_asr_fn fn,
                            void *userdata,
                            const char *name,
                            const char *version);

/* Register streaming ASR engine */
void wyoming_server_set_asr_streaming(wyoming_server_t *srv,
                                       wyoming_asr_stream_create_fn create_fn,
                                       wyoming_asr_stream_fn process_fn,
                                       wyoming_asr_stream_destroy_fn destroy_fn,
                                       void *userdata);

/* Add ASR model metadata (for describe response) */
wyoming_error_t wyoming_server_add_asr_model(wyoming_server_t *srv,
                                              const char *name,
                                              const char *const *languages,
                                              const char *description);
```

### 2.3 Server Protocol Dispatch (server.c handle_client additions)

#### Non-streaming ASR flow:
```
Client sends:  transcribe
Client sends:  audio-start  {rate, width, channels}
Client sends:  audio-chunk  (payload: PCM)  × N
Client sends:  audio-stop
Server calls:  asr_fn(accumulated_pcm, total_samples, &format, language, &text)
Server sends:  transcript   {text: "..."}
```

#### Streaming ASR flow:
```
Client sends:  transcribe-start  {language: "en"}
  Server calls: stream_create_fn(&format, language)
Client sends:  audio-chunk  (payload: PCM)
  Server calls: process_fn(chunk_pcm, chunk_samples, &format, 0, &partial)
  If partial != NULL:
    Server sends: transcript  {text: "partial...", is_partial: true}
Client sends:  audio-chunk  × N
  (same as above, partial results sent as available)
Client sends:  audio-stop
  Server calls: process_fn(NULL, 0, &format, 1, &final_text)
  Server sends: transcript  {text: "final result"}
  Server calls: stream_destroy_fn(stream_ctx)
```

### 2.4 Describe Response Update

```json
{
  "tts": [{
    "name": "piper",
    "version": "1.2.0",
    "installed": true,
    "voices": [
      {"name": "en_US-lessac-high", "languages": ["en_US"], "description": "lessac (high)"},
      {"name": "en_US-amy-medium", "languages": ["en_US"], "description": "amy (medium)"}
    ]
  }],
  "asr": [{
    "name": "sherpa-whisper",
    "version": "1.12.35",
    "installed": true,
    "models": [
      {"name": "whisper-base.en", "languages": ["en"], "description": "Whisper base (English)"},
      {"name": "whisper-small", "languages": ["auto"], "description": "Whisper small (multilingual)"}
    ]
  }]
}
```

---

## 3. Sherpa-ONNX Engine Integration (src/sherpa.c)

### 3.1 Dependencies

- **sherpa-onnx C API** (`libsherpa-onnx-c-api.so` + `libonnxruntime.so`)
- Pre-built aarch64 shared libs available: `sherpa-onnx-v1.12.35-linux-aarch64-shared-cpu-lib.tar.bz2` (11MB)
- No build from source needed — download, install to /usr/lib, done
- Header: `sherpa-onnx/c-api/c-api.h` (download from GitHub, install to /usr/include)

### 3.2 Engine API

```c
#ifdef WYOMING_HAVE_SHERPA

typedef struct wyoming_sherpa wyoming_sherpa_t;

/* Create offline (batch) recognizer from model files */
wyoming_sherpa_t *wyoming_sherpa_create(const char *model_dir,
                                        const char *model_type,  /* "whisper", "paraformer", "nemo_ctc" */
                                        const char *language);

/* Get callbacks for server registration */
wyoming_asr_fn              wyoming_sherpa_get_callback(void);
void                       *wyoming_sherpa_as_userdata(wyoming_sherpa_t *s);

/* Create streaming recognizer (for real-time ASR) */
wyoming_sherpa_t *wyoming_sherpa_create_streaming(const char *model_dir,
                                                    const char *model_type,
                                                    const char *language);

wyoming_asr_stream_create_fn  wyoming_sherpa_get_stream_create(void);
wyoming_asr_stream_fn         wyoming_sherpa_get_stream_process(void);
wyoming_asr_stream_destroy_fn wyoming_sherpa_get_stream_destroy(void);

/* Query */
const char *wyoming_sherpa_model_name(const wyoming_sherpa_t *s);
const char *wyoming_sherpa_language(const wyoming_sherpa_t *s);
int         wyoming_sherpa_sample_rate(const wyoming_sherpa_t *s);

void wyoming_sherpa_destroy(wyoming_sherpa_t *s);

#endif /* WYOMING_HAVE_SHERPA */
```

### 3.3 Internal Implementation

**Offline (batch) ASR** — wraps sherpa-onnx offline recognizer:
```c
static wyoming_error_t sherpa_asr_callback(
    const int16_t *pcm, size_t samples,
    const wyoming_audio_format_t *format,
    const char *language, char **text_out, void *userdata)
{
    wyoming_sherpa_t *s = userdata;
    /* Resample to 16kHz if needed (sherpa expects 16000) */
    /* Create offline stream */
    /* Accept waveform */
    /* Decode */
    /* Get result text */
    /* Return */
}
```

**Online (streaming) ASR** — wraps sherpa-onnx online recognizer:
```c
static void *sherpa_stream_create(const wyoming_audio_format_t *format,
                                   const char *language, void *userdata)
{
    /* Create online stream from the online recognizer */
    /* Return stream as opaque context */
}

static wyoming_error_t sherpa_stream_process(
    const int16_t *pcm, size_t samples,
    const wyoming_audio_format_t *format,
    int is_final, char **text_out, void *userdata)
{
    /* Accept waveform chunk */
    /* If IsOnlineStreamReady: decode, get partial result */
    /* If is_final: get final result */
}

static void sherpa_stream_destroy(void *stream_ctx, void *userdata)
{
    /* Destroy online stream */
}
```

### 3.4 Supported Model Types

| Model Type | Sherpa Config | Streaming | Notes |
|-----------|--------------|-----------|-------|
| Whisper (tiny/base/small/medium) | `OfflineWhisperModelConfig` | No (batch only) | Best accuracy, slower |
| Zipformer transducer | `OnlineTransducerModelConfig` | Yes | Fast, good accuracy, streaming |
| Paraformer | `OnlineParaformerModelConfig` | Yes | Alternative streaming |
| NeMo CTC | `OnlineNemoCtcModelConfig` | Yes | Low latency |
| Sense Voice | `OfflineSenseVoiceModelConfig` | No | Multilingual |

---

## 4. Standalone Server Binaries

### 4.1 wyoming-piper-server

**File:** `bin/wyoming-piper-server.c`

```
Usage: wyoming-piper-server [options]
  --uri tcp://0.0.0.0:10200     Listen address
  --model /path/to/model.onnx   Piper ONNX voice model
  --espeak-data /path/to/data   espeak-ng data directory
  --speaker NAME                Default speaker (multi-speaker models)
  --voice-name NAME             Voice name for describe response
```

Implementation:
1. Parse CLI args
2. `wyoming_piper_create(model_path, espeak_data_path)`
3. `wyoming_server_create(bind, port)`
4. `wyoming_server_set_tts(srv, wyoming_piper_get_callback(), wyoming_piper_as_userdata(piper), ...)`
5. `wyoming_server_add_voice(srv, name, languages, description)`
6. `wyoming_server_run(srv)` — blocks until SIGINT/SIGTERM

### 4.2 wyoming-asr-server

**File:** `bin/wyoming-asr-server.c`

```
Usage: wyoming-asr-server [options]
  --uri tcp://0.0.0.0:10300     Listen address
  --model-dir /path/to/model    Sherpa-ONNX model directory
  --model-type whisper           Model type: whisper, zipformer, paraformer, nemo_ctc
  --language en                 Language code (or "auto")
  --streaming                   Enable streaming ASR (requires streaming model)
  --cpu-threads 4               ONNX Runtime thread count
```

Implementation:
1. Parse CLI args
2. `wyoming_sherpa_create(model_dir, model_type, language)` (or `_streaming`)
3. `wyoming_server_create(bind, port)`
4. `wyoming_server_set_asr(srv, callback, userdata, name, version)`
5. If streaming: `wyoming_server_set_asr_streaming(srv, create, process, destroy, userdata)`
6. `wyoming_server_add_asr_model(srv, name, languages, description)`
7. `wyoming_server_run(srv)`

---

## 5. Debian Packaging

### 5.1 Package Layout

| Package | Contents | Depends |
|---------|----------|---------|
| `libwyoming0` | `libwyoming.so.0` | libc |
| `libwyoming-dev` | headers, .pc, .a, .la | libwyoming0 |
| `wyoming-piper-server` | `/usr/bin/wyoming-piper-server`, systemd unit | libwyoming0, libpiper-phonemize, libonnxruntime, libespeak-ng |
| `wyoming-asr-server` | `/usr/bin/wyoming-asr-server`, systemd unit | libwyoming0, sherpa-onnx-libs |
| `sherpa-onnx-libs` | `libsherpa-onnx-c-api.so`, `libonnxruntime.so`, header | (new package, wraps upstream tarball) |
| `wyoming-voice-en-us-lessac-high` | Model .onnx + .onnx.json in /usr/share/wyoming/voices/ | wyoming-piper-server |
| `wyoming-voice-en-us-amy-medium` | Model .onnx + .onnx.json | wyoming-piper-server |
| `wyoming-voice-en-gb-alba-medium` | Model .onnx + .onnx.json | wyoming-piper-server |
| `wyoming-model-whisper-base-en` | Whisper base.en CTranslate2 model files in /usr/share/wyoming/models/ | wyoming-asr-server |
| `wyoming-model-whisper-small` | Whisper small multilingual | wyoming-asr-server |
| `wyoming-model-zipformer-en` | Streaming zipformer English | wyoming-asr-server |

### 5.2 Voice Data Packages (TTS)

Source: https://huggingface.co/rhasspy/piper-voices
153 voices across 30+ languages.

Each voice package:
- Installs to `/usr/share/wyoming/voices/<name>/`
- Contains: `model.onnx`, `model.onnx.json` (config with sample_rate, phoneme_type)
- Package name: `wyoming-voice-<lang>-<name>-<quality>` (e.g., `wyoming-voice-en-us-lessac-high`)
- Priority voices to package first:
  - `en_US-lessac-high` (22050 Hz, high quality, English US)
  - `en_US-amy-medium` (22050 Hz, medium quality)
  - `en_US-arctic-medium` (multi-speaker, 18 speakers)
  - `en_GB-alba-medium` (British English)
  - `es_ES-davefx-medium` (Spanish)
  - `de_DE-thorsten-high` (German)
  - `fr_FR-siwis-medium` (French)

### 5.3 ASR Model Packages

Source: https://github.com/k2-fsa/sherpa-onnx/releases (model archives)

Each model package:
- Installs to `/usr/share/wyoming/models/<name>/`
- Contains: encoder.onnx, decoder.onnx, tokens.txt, and model-specific files
- Package name: `wyoming-model-<type>-<variant>` (e.g., `wyoming-model-whisper-base-en`)

Priority models:
- **whisper-base.en** — 74MB, English only, best accuracy/size tradeoff for batch ASR
- **whisper-small** — 244MB, multilingual, higher accuracy
- **whisper-tiny.en** — 39MB, fastest, lower accuracy
- **sherpa-onnx-streaming-zipformer-en-2023-06-26** — streaming English, low latency
- **sherpa-onnx-streaming-paraformer-bilingual-zh-en** — streaming Chinese+English

### 5.4 Systemd Service Files

#### `/lib/systemd/system/wyoming-piper.service`
```ini
[Unit]
Description=Wyoming Piper TTS Server
After=network.target
Documentation=https://github.com/briankwest/libwyoming

[Service]
Type=simple
User=wyoming
Group=wyoming
ExecStart=/usr/bin/wyoming-piper-server \
    --uri tcp://0.0.0.0:10200 \
    --model /usr/share/wyoming/voices/en_US-lessac-high/model.onnx \
    --espeak-data /usr/share/espeak-ng-data
EnvironmentFile=-/etc/default/wyoming-piper
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadOnlyPaths=/usr/share/wyoming

[Install]
WantedBy=multi-user.target
```

#### `/lib/systemd/system/wyoming-asr.service`
```ini
[Unit]
Description=Wyoming ASR Server (Sherpa-ONNX)
After=network.target
Documentation=https://github.com/briankwest/libwyoming

[Service]
Type=simple
User=wyoming
Group=wyoming
ExecStart=/usr/bin/wyoming-asr-server \
    --uri tcp://0.0.0.0:10300 \
    --model-dir /usr/share/wyoming/models/whisper-base.en \
    --model-type whisper \
    --language en \
    --cpu-threads 4
EnvironmentFile=-/etc/default/wyoming-asr
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadOnlyPaths=/usr/share/wyoming

[Install]
WantedBy=multi-user.target
```

#### `/etc/default/wyoming-piper` (override file)
```bash
# Override voice model, port, etc.
# WYOMING_PIPER_OPTS="--model /usr/share/wyoming/voices/en_US-amy-medium/model.onnx"
```

#### `/etc/default/wyoming-asr`
```bash
# Override model, threads, etc.
# WYOMING_ASR_OPTS="--model-dir /usr/share/wyoming/models/whisper-small --cpu-threads 8"
```

---

## 6. Build System Changes

### 6.1 configure.ac additions

```
# Optional: sherpa-onnx (for native ASR server)
AC_ARG_WITH([sherpa-onnx],
  [AS_HELP_STRING([--with-sherpa-onnx=DIR], [sherpa-onnx install prefix])],
  [SHERPA_CFLAGS="-I$withval/include"
   SHERPA_LIBS="-L$withval/lib -lsherpa-onnx-c-api -lonnxruntime"
   have_sherpa=yes],
  [have_sherpa=no])
AM_CONDITIONAL([HAVE_SHERPA], [test "x$have_sherpa" = "xyes"])
if test "x$have_sherpa" = "xyes"; then
  AC_DEFINE([WYOMING_HAVE_SHERPA], [1], [Have sherpa-onnx])
  AC_SUBST([SHERPA_CFLAGS])
  AC_SUBST([SHERPA_LIBS])
fi

# Optional: piper_phonemize (for native TTS server)
# (already exists as WYOMING_HAVE_PIPER)
```

### 6.2 Makefile.am additions

```makefile
# Sherpa engine (conditional)
if HAVE_SHERPA
libwyoming_la_SOURCES += src/sherpa.c
libwyoming_la_CFLAGS  += $(SHERPA_CFLAGS)
libwyoming_la_LIBADD  += $(SHERPA_LIBS)
endif

# Server binaries
bin_PROGRAMS =

if HAVE_PIPER
bin_PROGRAMS += wyoming-piper-server
wyoming_piper_server_SOURCES = bin/wyoming-piper-server.c
wyoming_piper_server_LDADD = src/libwyoming.la $(PIPER_LIBS)
endif

if HAVE_SHERPA
bin_PROGRAMS += wyoming-asr-server
wyoming_asr_server_SOURCES = bin/wyoming-asr-server.c
wyoming_asr_server_LDADD = src/libwyoming.la $(SHERPA_LIBS)
endif
```

### 6.3 Debian packaging additions

```
debian/
├── control                    # Updated with new binary packages
├── wyoming-piper-server.install
├── wyoming-piper-server.service
├── wyoming-asr-server.install
├── wyoming-asr-server.service
├── wyoming-piper-server.default
├── wyoming-asr-server.default
└── rules                      # Build both server binaries
```

Additional per-model repos (or a model-packager script):
```
wyoming-voices/                # Separate repo for voice packaging
├── download-voices.sh         # Downloads from HuggingFace
├── debian/
│   ├── control                # One stanza per voice package
│   ├── wyoming-voice-en-us-lessac-high.install
│   ├── wyoming-voice-en-us-amy-medium.install
│   └── ...
└── voices/                    # Downloaded .onnx files
```

---

## 7. Directory Layout (installed)

```
/usr/lib/
├── libwyoming.so.0.0.0
├── libsherpa-onnx-c-api.so      (from sherpa-onnx-libs package)
└── libonnxruntime.so             (from sherpa-onnx-libs package)

/usr/bin/
├── wyoming-piper-server
├── wyoming-asr-server
└── wyoming-describe              (CLI tool: describe any Wyoming server)

/usr/include/libwyoming/
├── wyoming.h
└── version.h

/usr/share/wyoming/
├── voices/
│   ├── en_US-lessac-high/
│   │   ├── model.onnx
│   │   └── model.onnx.json
│   ├── en_US-amy-medium/
│   │   ├── model.onnx
│   │   └── model.onnx.json
│   └── ...
└── models/
    ├── whisper-base.en/
    │   ├── encoder.onnx
    │   ├── decoder.onnx
    │   └── tokens.txt
    ├── whisper-small/
    │   ├── encoder.onnx
    │   ├── decoder.onnx
    │   └── tokens.txt
    └── zipformer-streaming-en/
        ├── encoder.onnx
        ├── decoder.onnx
        ├── joiner.onnx
        └── tokens.txt

/lib/systemd/system/
├── wyoming-piper.service
└── wyoming-asr.service

/etc/default/
├── wyoming-piper
└── wyoming-asr
```

---

## 8. Implementation Order

### Phase 1: Server ASR support (libwyoming)
1. Add `wyoming_asr_fn` typedef and `wyoming_server_set_asr()` to header
2. Add ASR fields to `wyoming_server` struct
3. Add `transcribe` dispatch to `handle_client()` (non-streaming)
4. Update `build_info_data()` for ASR in describe response
5. Add `wyoming_server_add_asr_model()` metadata API
6. Tests: unit test for ASR server dispatch

### Phase 2: Streaming ASR protocol
1. Add streaming callback types to header
2. Add `wyoming_server_set_asr_streaming()` API
3. Add `transcribe-start` / `audio-chunk` / `audio-stop` streaming dispatch
4. Send partial `transcript` events during streaming
5. Add streaming client functions: `wyoming_transcribe_start()`, `wyoming_transcribe_chunk()`, `wyoming_transcribe_stop()`
6. Tests: streaming ASR round-trip

### Phase 3: Sherpa-ONNX engine (src/sherpa.c)
1. Package `sherpa-onnx-libs` deb (shared libs + header from upstream tarball)
2. Add `--with-sherpa-onnx` to configure.ac
3. Implement `wyoming_sherpa_create()` — offline Whisper recognizer
4. Implement `wyoming_sherpa_create_streaming()` — online zipformer recognizer
5. Implement batch ASR callback wrapping SherpaOnnx offline API
6. Implement streaming ASR callbacks wrapping SherpaOnnx online API
7. Tests: Whisper model load + transcribe, streaming transcribe

### Phase 4: Server binaries
1. `bin/wyoming-piper-server.c` — CLI arg parsing, Piper engine init, server run
2. `bin/wyoming-asr-server.c` — CLI arg parsing, Sherpa engine init, server run
3. `bin/wyoming-describe.c` — CLI tool to query any Wyoming server
4. Systemd service files
5. `/etc/default/` override files
6. Makefile.am conditional builds

### Phase 5: Debian packaging
1. Update `debian/control` with new binary packages
2. `.install` files for each binary package
3. `.service` files for systemd
4. `.default` files for env overrides
5. `postinst`/`prerm` scripts for service management
6. Build and test: `dpkg-buildpackage`

### Phase 6: Model/voice data packages
1. Create `wyoming-voices` repo with packaging scripts
2. `download-voices.sh` — fetches from HuggingFace, organizes by name
3. `package-voice.sh <name>` — creates a deb for one voice
4. Priority voices: en_US-lessac-high, en_US-amy-medium, en_GB-alba-medium
5. Create `wyoming-models` repo with ASR model packaging
6. `download-model.sh` — fetches from sherpa-onnx releases
7. `package-model.sh <name>` — creates a deb for one model
8. Priority models: whisper-base.en, whisper-tiny.en, zipformer-streaming-en

### Phase 7: kerchunk integration updates
1. mod_tts: no changes needed (already uses Wyoming client)
2. mod_asr: no changes needed (already uses Wyoming client)
3. kerchunk.conf.example: update docs to reference deb packages
4. CLAUDE.md: update dependency list
5. update.sh: add wyoming server rebuild

---

## 9. Compatibility Matrix

| Wyoming Client | wyoming-piper (Python) | wyoming-piper-server (C) | wyoming-faster-whisper (Python) | wyoming-asr-server (C) |
|---------------|----------------------|--------------------------|-------------------------------|----------------------|
| Home Assistant | ✓ | ✓ | ✓ | ✓ |
| kerchunk mod_tts | ✓ | ✓ | — | — |
| kerchunk mod_asr | — | — | ✓ | ✓ |
| Any Wyoming client | ✓ | ✓ | ✓ | ✓ |

All servers speak the same Wyoming protocol — the C servers are drop-in replacements
for the Python ones. Home Assistant, kerchunk, and any other Wyoming client work with either.

---

## 10. Hardware Requirements

| Component | CPU | RAM | Disk |
|-----------|-----|-----|------|
| wyoming-piper-server (1 voice) | 1 core | 200MB | 100MB (model) |
| wyoming-asr-server (whisper-base.en) | 4 cores | 500MB | 150MB (model) |
| wyoming-asr-server (whisper-small) | 4 cores | 1.5GB | 500MB (model) |
| wyoming-asr-server (zipformer streaming) | 2 cores | 300MB | 80MB (model) |
| Both servers + kerchunk | 6 cores | 2GB | 500MB |

The Cortex-A720/A520 server (12 cores, 60GB RAM) can comfortably run all of this
simultaneously with room to spare.
