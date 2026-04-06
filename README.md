# libwyoming

C library implementing the [Wyoming voice protocol](https://github.com/OHF-Voice/wyoming) used by Home Assistant for TTS, ASR, and other voice services.

Provides client and server APIs over TCP with JSON-line framing and binary audio payloads, plus a standalone Piper TTS server.

## Features

- **Protocol layer** — buffered event reader/writer with JSON-line headers + binary payloads
- **Client API** — connect, synthesize, synthesize\_pcm (high-level), describe
- **Server API** — epoll event loop with pluggable TTS callback and signal-safe shutdown
- **Piper TTS engine** — subprocess wrapper for [Piper](https://github.com/rhasspy/piper) with `--output_raw`
- **Standalone server** — `wyoming-piper-server --model X --port Y`
- **Zero external dependencies** — cJSON vendored, no runtime deps beyond libc

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

## Installation

### From source

```bash
sudo make install
sudo ldconfig
```

### From Debian packages

```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../libwyoming0_*.deb ../libwyoming-dev_*.deb
sudo ldconfig
```

Two packages are produced:

| Package | Contents |
|---------|----------|
| `libwyoming0` | Runtime shared library (`libwyoming.so.0`) |
| `libwyoming-dev` | Headers, static library, pkg-config file |

## Usage

### pkg-config

```bash
gcc $(pkg-config --cflags libwyoming) myapp.c $(pkg-config --libs libwyoming)
```

### Client example

```c
#include <libwyoming/wyoming.h>

wyoming_conn_t *conn = wyoming_connect("localhost", 10200);

/* High-level: send text, get back complete PCM buffer */
int16_t *pcm;
size_t samples;
wyoming_audio_format_t fmt;
wyoming_synthesize_pcm(conn, "Hello world", "en_US-lessac-high",
                       &pcm, &samples, &fmt);
/* pcm contains signed 16-bit mono samples at fmt.rate Hz */
free(pcm);

/* List available voices */
wyoming_info_t info;
wyoming_describe(conn, &info);
for (int i = 0; i < info.tts[0].voice_count; i++)
    printf("  %s\n", info.tts[0].voices[i].name);
wyoming_info_free(&info);

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

## Wyoming Piper Server

Standalone binary that serves a Piper model over the Wyoming protocol:

```bash
wyoming-piper-server \
  --model /usr/share/piper/models/en_US-lessac-high.onnx \
  --port 10200
```

Any Wyoming client — including Home Assistant — can connect and request TTS synthesis.

### Options

| Flag | Description |
|------|-------------|
| `--model PATH` | Path to Piper `.onnx` model file (required) |
| `--port PORT` | TCP port to listen on (required) |
| `--host ADDR` | Bind address (default: `0.0.0.0`) |
| `--piper PATH` | Path to piper binary (default: `piper`) |

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

### Service discovery

1. Client sends `describe`
2. Server responds with `info` (lists available TTS engines and voices)

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
wyoming_error_t wyoming_read_event(wyoming_conn_t *conn, wyoming_event_t *event);
wyoming_error_t wyoming_write_event(wyoming_conn_t *conn, const wyoming_event_t *event);
wyoming_error_t wyoming_synthesize(wyoming_conn_t *conn, const char *text, const char *voice);
wyoming_error_t wyoming_synthesize_pcm(wyoming_conn_t *conn, const char *text, const char *voice,
                                       int16_t **pcm_out, size_t *samples_out,
                                       wyoming_audio_format_t *format_out);
wyoming_error_t wyoming_describe(wyoming_conn_t *conn, wyoming_info_t *info_out);
void            wyoming_close(wyoming_conn_t *conn);
```

### Server functions

```c
wyoming_server_t *wyoming_server_create(const char *bind_addr, uint16_t port);
void              wyoming_server_set_tts(wyoming_server_t *srv, wyoming_tts_fn fn,
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
