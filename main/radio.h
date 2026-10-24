#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HTTP(S) audio receiver: a network task downloads a stream into a PSRAM ring buffer, the
 * player decodes from it. Used for the internet radio and for DLNA casting. */

#define STREAM_URL_SIZE 1024 /* cast URLs from phones carry long tokens */

typedef enum {
    RADIO_STOPPED,
    RADIO_CONNECTING,
    RADIO_STREAMING, /* connected, data is flowing into the buffer */
    RADIO_ERROR,     /* see error; a live stream is retried */
    RADIO_ENDED,     /* a file was received completely */
} radio_state_t;

typedef enum {
    RADIO_CODEC_MP3,
    RADIO_CODEC_AAC,
    RADIO_CODEC_FLAC,
    RADIO_CODEC_WAV,
    RADIO_CODEC_M4A,
} radio_codec_t;

typedef struct {
    radio_state_t state;
    uint32_t generation; /* request this status belongs to, see radio_play() */
    radio_codec_t codec;
    size_t buffered;     /* bytes waiting in the buffer */
    size_t buffer_size;
    uint32_t content_length; /* whole file size, 0 when unknown (live stream) */
    bool seekable;           /* the server serves byte ranges */
    char error[96];
} radio_status_t;

void radio_init(void);

/* Switch to a stream (empty url stops). live: a radio station, reconnected forever; otherwise
 * a file, received once (RADIO_ENDED / RADIO_ERROR), from byte offset on. mime_hint (may be
 * NULL) helps when the server does not tell the format. Returns the new request generation;
 * buffered data belongs to it once radio_get_status() reports this generation. */
uint32_t radio_play(const char *url, const char *mime_hint, bool live, uint32_t offset);
void radio_stop(void);
uint32_t radio_generation(void);

void radio_get_status(radio_status_t *out);

/* Take up to len buffered bytes, never blocks */
size_t radio_read(uint8_t *buf, size_t len);

struct cJSON;
/* "radio" of /api/status */
void radio_status_json(struct cJSON *out);
