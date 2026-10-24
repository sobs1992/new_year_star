#pragma once

#include <stddef.h>
#include <stdint.h>

/* Internet radio receiver: a network task downloads the stream into a PSRAM ring buffer,
 * the player decodes from it. */

typedef enum {
    RADIO_STOPPED,
    RADIO_CONNECTING,
    RADIO_STREAMING, /* connected, data is flowing into the buffer */
    RADIO_ERROR,     /* will retry, see error */
} radio_state_t;

typedef enum {
    RADIO_CODEC_MP3,
    RADIO_CODEC_AAC,
} radio_codec_t;

typedef struct {
    radio_state_t state;
    uint32_t generation; /* request this status belongs to, see radio_play() */
    radio_codec_t codec;
    size_t buffered;     /* bytes waiting in the buffer */
    size_t buffer_size;
    char error[96];
} radio_status_t;

void radio_init(void);

/* Switch to a stream (empty url stops). Returns the new request generation; buffered data
 * belongs to it once radio_get_status() reports this generation. */
uint32_t radio_play(const char *url);
void radio_stop(void);
uint32_t radio_generation(void);

void radio_get_status(radio_status_t *out);

/* Take up to len buffered bytes, never blocks */
size_t radio_read(uint8_t *buf, size_t len);
