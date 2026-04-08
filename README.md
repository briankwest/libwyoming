# libwyoming

C library implementing the [Wyoming voice protocol](https://github.com/OHF-Voice/wyoming) used by Home Assistant for TTS, ASR, and other voice services.

Provides client and server APIs over TCP with JSON-line framing and binary audio payloads, plus a combined TTS/ASR server daemon with native Piper and Sherpa-ONNX engines.

## Features

- **Protocol layer** — buffered event reader/writer with JSON-line headers + binary payloads
- **Client API** — connect, synthesize, synthesize\_pcm, transcribe\_pcm, describe, streaming transcribe
- **Server API** — epoll event loop with pluggable TTS and ASR callbacks, signal-safe shutdown
- **Piper TTS** — native C++ engine (libpiper\_phonemize + onnxruntime) or subprocess fallback
- **Sherpa-ONNX ASR** — batch (Whisper offline) and streaming (Zipformer online) speech recognition
- **Combined server** — `wyoming-server` daemon with INI config, serving TTS + ASR on one port
- **CLI tools** — `wyoming-describe` (query server), `wyoming-speak` (synthesize text to WAV)
- **Voice/model packages** — scripts to build `.deb` packages for Piper voices and Sherpa-ONNX models
- **Zero required dependencies** — cJSON vendored; Piper and Sherpa-ONNX are optional at build time

## Building

```bash
./autogen.sh
./configure
make
make check        # run tests
```

### Build dependencies

```bash
sudo apt-get install build-essential pkg-config autoconf automake libtool
```

### Optional engine dependencies

These enable native TTS and ASR engines in the combined server. Without them, the library still builds — only the engine features are disabled.

#### Piper TTS (native engine)

Requires libpiper\_phonemize, onnxruntime 1.14.x, and espeak-ng:

```bash
# espeak-ng (from apt)
sudo apt-get install espeak-ng libespeak-ng-dev

# onnxruntime 1.14.1 (from GitHub releases)
wget https://github.com/microsoft/onnxruntime/releases/download/v1.14.1/onnxruntime-linux-aarch64-1.14.1.tgz
tar xzf onnxruntime-linux-aarch64-1.14.1.tgz
sudo cp onnxruntime-linux-aarch64-1.14.1/lib/libonnxruntime.so* /usr/lib/
sudo cp -r onnxruntime-linux-aarch64-1.14.1/include/* /usr/include/
sudo ldconfig

# libpiper_phonemize (from source)
git clone https://github.com/rhasspy/piper-phonemize
cd piper-phonemize
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

For x86\_64, replace `aarch64` with `x64` in the onnxruntime URL.

#### Sherpa-ONNX ASR

Requires the sherpa-onnx C API library:

```bash
git clone https://github.com/k2-fsa/sherpa-onnx
cd sherpa-onnx
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr \
      -DSHERPA_ONNX_ENABLE_C_API=ON \
      -DBUILD_SHARED_LIBS=ON ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Verifying optional features

After `./configure`, check the summary output:

```
libwyoming 0.1.0 configuration:
  Native Piper TTS:  yes    (libpiper_phonemize + onnxruntime)
  Sherpa-ONNX ASR:   yes    (libsherpa-onnx-c-api)
```

## Installation

### From source

```bash
sudo make install
sudo ldconfig
```

### From Debian packages

```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../libwyoming0_*.deb ../libwyoming-dev_*.deb ../wyoming-tools_*.deb
sudo ldconfig
```

Three packages are produced:

| Package | Contents |
|---------|----------|
| `libwyoming0` | Runtime shared library (`libwyoming.so.0`) |
| `libwyoming-dev` | Headers, static library, pkg-config file |
| `wyoming-tools` | `wyoming-server`, `wyoming-describe`, `wyoming-speak` binaries + systemd service |

## Wyoming Server

Combined TTS + ASR daemon with INI configuration:

```bash
wyoming-server -c /etc/wyoming/wyoming-server.conf
```

### Configuration

```ini
[server]
host = 0.0.0.0
port = 10200

[tts]
engine = piper-native          # or "piper" for subprocess mode
model = /usr/share/wyoming/voices/en_US-lessac-high/model.onnx

[asr]
engine = sherpa
model_dir = /usr/share/wyoming/models/zipformer-en
model_type = zipformer          # or "whisper"
language = en
streaming = on                  # real-time transcription

[log]
level = info
file = /var/log/wyoming-server.log
```

### TTS engines

| Engine | Config value | Description |
|--------|-------------|-------------|
| Native Piper | `piper-native` | Direct C++ calls to libpiper\_phonemize + onnxruntime. Fast, no subprocess overhead. Requires build with `--enable-piper`. |
| Piper subprocess | `piper` | Spawns `/usr/bin/piper` per utterance. Works without native deps. Set `piper_binary` if not in default path. |

### ASR models

| Model type | Config value | Description |
|-----------|-------------|-------------|
| Zipformer | `zipformer` | Streaming ASR — real-time transcription during speech. Small, fast. |
| Whisper | `whisper` | Batch ASR — transcribes after speech ends. Higher accuracy. |

### systemd

The `wyoming-tools` package installs a systemd service:

```bash
sudo systemctl enable --now wyoming-server
```

## CLI Tools

### wyoming-describe

Query a Wyoming server's capabilities:

```bash
wyoming-describe localhost 10200
```

### wyoming-speak

Synthesize text to a WAV file:

```bash
wyoming-speak localhost 10200 "Hello world" output.wav
```

## Voice and Model Packages

Scripts in `data/` download Piper voices and Sherpa-ONNX models from upstream and build `.deb` packages:

```bash
# Package a Piper voice
./data/package-voice.sh en_US-lessac-high
sudo dpkg -i wyoming-voice-en-us-lessac-high_1.0.0_all.deb

# Package a Sherpa-ONNX ASR model
./data/package-model.sh zipformer-en
sudo dpkg -i wyoming-model-zipformer-en_1.0.0_all.deb

# Build all priority packages
./data/build-all.sh
```

Voices install to `/usr/share/wyoming/voices/<name>/`, models to `/usr/share/wyoming/models/<name>/`.

## Usage

### pkg-config

```bash
gcc $(pkg-config --cflags libwyoming) myapp.c $(pkg-config --libs libwyoming)
```

### Client example

```c
#include <libwyoming/wyoming.h>

wyoming_conn_t *conn = wyoming_connect("localhost", 10200);

/* TTS: send text, get back complete PCM buffer */
int16_t *pcm;
size_t samples;
wyoming_audio_format_t fmt;
wyoming_synthesize_pcm(conn, "Hello world", "en_US-lessac-high",
                       &pcm, &samples, &fmt);
/* pcm contains signed 16-bit mono samples at fmt.rate Hz */
free(pcm);
wyoming_close(conn);

/* ASR: send audio, get back text */
conn = wyoming_connect("localhost", 10200);
char *text = NULL;
wyoming_audio_format_t afmt = { .rate = 16000, .width = 2, .channels = 1 };
wyoming_transcribe_pcm(conn, audio_buf, num_samples, &afmt, "en", &text);
printf("Transcript: %s\n", text);
free(text);
wyoming_close(conn);
```

### Server example

```c
#include <libwyoming/wyoming.h>

static wyoming_error_t my_tts(const char *text, const char *voice,
                              const char *speaker, int16_t **pcm_out,
                              size_t *samples_out,
                              wyoming_audio_format_t *format_out,
                              void *userdata) {
    /* Synthesize text into PCM here */
    return WYOMING_OK;
}

wyoming_server_t *srv = wyoming_server_create("0.0.0.0", 10200);
wyoming_server_set_tts(srv, my_tts, NULL, "my-engine", "1.0");
const char *langs[] = {"en_US", NULL};
wyoming_server_add_voice(srv, "my-voice", langs, "My custom voice");
wyoming_server_run(srv);  /* blocks until wyoming_server_stop() */
wyoming_server_destroy(srv);
```

## Wyoming Protocol

The wire format is newline-delimited JSON headers followed by optional data and binary payload:

```
{"type":"synthesize","data_length":42}\n
{"text":"Hello","voice":{"name":"en_US-lessac-high"}}
```

```
{"type":"audio-chunk","data_length":28,"payload_length":8192}\n
{"rate":22050,"width":2,"channels":1}
<8192 bytes of PCM16LE audio>
```

### TTS flow

1. Client sends `synthesize` event (text + optional voice)
2. Server responds with `audio-start` (sample rate, width, channels)
3. Server sends N `audio-chunk` events with PCM payload
4. Server sends `audio-stop`

### ASR flow (batch)

1. Client sends `transcribe` event (language)
2. Client sends `audio-start` + N `audio-chunk` events + `audio-stop`
3. Server responds with `transcript` (text)

### ASR flow (streaming)

1. Client sends `transcribe` event (language)
2. Client sends `audio-start`
3. Client sends `audio-chunk` events as audio is captured
4. Server may send `transcript` events with partial results
5. Client sends `audio-stop`
6. Server responds with final `transcript`

### Service discovery

1. Client sends `describe`
2. Server responds with `info` (lists available TTS engines/voices and ASR models)

## API Reference

### Error codes

| Code | Meaning |
|------|---------|
| `WYOMING_OK` | Success |
| `WYOMING_ERR_IO` | Read/write/socket failure |
| `WYOMING_ERR_PARSE` | Malformed JSON |
| `WYOMING_ERR_NOMEM` | Allocation failure |
| `WYOMING_ERR_PROTO` | Unexpected event type |
| `WYOMING_ERR_EOF` | Peer closed connection |
| `WYOMING_ERR_TIMEOUT` | Operation timed out |
| `WYOMING_ERR_PIPER` | Piper engine error |
| `WYOMING_ERR_INVAL` | Invalid argument |

### Client functions

```c
wyoming_conn_t *wyoming_connect(const char *host, uint16_t port);
wyoming_error_t wyoming_synthesize(wyoming_conn_t *conn, const char *text, const char *voice);
wyoming_error_t wyoming_synthesize_pcm(wyoming_conn_t *conn, const char *text, const char *voice,
                                       int16_t **pcm_out, size_t *samples_out,
                                       wyoming_audio_format_t *format_out);
wyoming_error_t wyoming_transcribe_pcm(wyoming_conn_t *conn, const int16_t *pcm, size_t samples,
                                        const wyoming_audio_format_t *format,
                                        const char *language, char **text_out);
wyoming_error_t wyoming_transcribe_start(wyoming_conn_t *conn,
                                          const wyoming_audio_format_t *format,
                                          const char *language);
wyoming_error_t wyoming_transcribe_chunk(wyoming_conn_t *conn, const int16_t *pcm, size_t samples,
                                          const wyoming_audio_format_t *format);
wyoming_error_t wyoming_transcribe_stop(wyoming_conn_t *conn, char **text_out);
wyoming_error_t wyoming_describe(wyoming_conn_t *conn, wyoming_info_t *info_out);
void            wyoming_close(wyoming_conn_t *conn);
```

### Server functions

```c
wyoming_server_t *wyoming_server_create(const char *bind_addr, uint16_t port);
void              wyoming_server_set_tts(wyoming_server_t *srv, wyoming_tts_fn fn,
                                         void *userdata, const char *name, const char *version);
void              wyoming_server_set_asr(wyoming_server_t *srv, wyoming_asr_fn fn,
                                         void *userdata, const char *name, const char *version);
wyoming_error_t   wyoming_server_add_voice(wyoming_server_t *srv, const char *name,
                                            const char *const *languages, const char *description);
wyoming_error_t   wyoming_server_add_speaker(wyoming_server_t *srv, const char *speaker_name);
wyoming_error_t   wyoming_server_run(wyoming_server_t *srv);
void              wyoming_server_stop(wyoming_server_t *srv);
uint16_t          wyoming_server_port(wyoming_server_t *srv);
void              wyoming_server_destroy(wyoming_server_t *srv);
```

## License

MIT — see [LICENSE](LICENSE).

cJSON is vendored under its own MIT license — see [src/cJSON/cJSON.c](src/cJSON/cJSON.c).
