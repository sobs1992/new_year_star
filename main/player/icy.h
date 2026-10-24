#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ICY metadata of a radio stream (Shoutcast / Icecast). Asked for with "Icy-MetaData: 1", the
 * server puts a block into the audio every icy-metaint bytes: a length byte (times 16), then
 * "StreamTitle='Artist - Song';StreamUrl='...';" padded with zeros. A length of 0 means no
 * change. Parsing only, no I/O: the host tests run it. */

#define ICY_TITLE_SIZE 192 /* bytes incl. '\0', UTF-8 */

/* title: UTF-8, may be empty (a jingle). audio_before: audio bytes of the buffer given to
 * icy_strip() that come before the title, in the stripped buffer. */
typedef void (*icy_title_fn)(const char *title, size_t audio_before);

typedef struct {
    uint32_t metaint;    /* audio bytes between blocks, 0: the server sends none */
    uint32_t audio_left; /* audio bytes until the next length byte */
    uint32_t meta_left;  /* bytes of the current block still to come */
    size_t meta_len;     /* bytes of the block kept in meta */
    char meta[256];      /* the start of the block: StreamTitle comes first */
    char title[ICY_TITLE_SIZE];
} icy_t;

/* At the start of a response, with its icy-metaint (0: none) */
void icy_init(icy_t *icy, uint32_t metaint);

/* Takes the metadata out of received bytes in place and returns how many audio bytes are left
 * at the start of buf. Blocks with a StreamTitle call on_title. */
size_t icy_strip(icy_t *icy, uint8_t *buf, size_t len, icy_title_fn on_title);

/* The StreamTitle of a block, trimmed, as UTF-8: Windows-1251 when it is not valid UTF-8 (old
 * Russian servers), cut at a character boundary. False when the block has no StreamTitle. */
bool icy_parse_title(const char *meta, char out[ICY_TITLE_SIZE]);
