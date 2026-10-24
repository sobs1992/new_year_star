#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_audio_simple_dec.h"

/* HTTP(S) audio receiver: a network task downloads a stream into a PSRAM ring buffer, the
 * player decodes from it. Used for the internet radio and for DLNA casting. */

#define STREAM_URL_SIZE 1024 /* cast URLs from phones carry long tokens */

typedef enum {
    STREAM_STOPPED,
    STREAM_CONNECTING,
    STREAM_STREAMING, /* connected, data is flowing into the buffer */
    STREAM_ERROR,     /* see error; a live stream is retried */
    STREAM_ENDED,     /* a file was received completely */
} stream_state_t;

typedef enum {
    STREAM_CODEC_MP3,
    STREAM_CODEC_AAC,
    STREAM_CODEC_FLAC,
    STREAM_CODEC_WAV,
    STREAM_CODEC_M4A,
    STREAM_CODEC_COUNT,
} stream_codec_t;

/* The decoder that plays a format */
esp_audio_simple_dec_type_t stream_decoder(stream_codec_t codec);
/* Playing can start mid-file (seeking): raw MP3 and ADTS AAC frames are found anywhere, FLAC,
 * WAV and M4A need the header at the start of the file */
bool stream_resyncs(stream_codec_t codec);

typedef struct {
    stream_state_t state;
    uint32_t generation; /* request this status belongs to, see stream_play() */
    stream_codec_t codec;
    size_t buffered;     /* bytes waiting in the buffer */
    size_t buffer_size;
    uint32_t content_length; /* whole file size, 0 when unknown (live stream) */
    bool seekable;           /* the server serves byte ranges */
    char error[96];
} stream_status_t;

/* Allocate the buffer, start the network task */
void stream_start(void);

/* Switch to a stream (empty url stops). live: a radio station, reconnected forever; otherwise
 * a file, received once (STREAM_ENDED / STREAM_ERROR), from byte offset on. mime_hint (may be
 * NULL) helps when the server does not tell the format. Returns the new request generation;
 * buffered data belongs to it once stream_get_status() reports this generation. */
uint32_t stream_play(const char *url, const char *mime_hint, bool live, uint32_t offset);
void stream_stop(void);
uint32_t stream_generation(void);

void stream_get_status(stream_status_t *out);

/* Take up to len buffered bytes, never blocks */
size_t stream_read(uint8_t *buf, size_t len);

struct cJSON;
/* "radio" of /api/status; "title": the song a station is playing now, from its ICY metadata */
void stream_status_json(struct cJSON *out);
