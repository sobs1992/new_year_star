#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "stream.h"

/* DLNA / UPnP media renderer: phones find the star over SSDP (home network and the star's
 * own network) and send it the URL of a file to play. */

typedef struct {
    uint32_t generation; /* changes with every new URL, seek, and on Play after Stop */
    bool paused;
    uint32_t start_ms;    /* position the stream starts at (after a seek) */
    uint32_t byte_offset; /* and the byte to request it from */
    char url[STREAM_URL_SIZE];
    char mime[48];       /* from the DIDL-Lite protocolInfo, may be empty */
    char title[64];
} dlna_job_t;

/* Register the description / control / event handlers and start SSDP */
void dlna_start(httpd_handle_t httpd);

/* Switched off, the renderer leaves the network (SSDP byebye) and a session ends; may come
 * before dlna_start() */
void dlna_set_enabled(bool on);

/* What the phone wants played: true while playing or paused. out may be NULL. */
bool dlna_get_job(dlna_job_t *out);

/* Playing, paused, or a phone sent a transport command a moment ago. Switching tracks is
 * Stop, SetAVTransportURI, Play: in between the star must not go back to its own playlist or
 * radio (a radio TLS handshake would hold up the next track for seconds). */
bool dlna_session_open(void);

/* From the player: progress, what is known about the file (seekable: byte ranges work and
 * the format can start mid-file), and the end of a job (failed: could not be played) */
void dlna_set_position(uint32_t generation, uint32_t ms);
void dlna_set_stream_info(uint32_t generation, uint32_t content_length, uint32_t bitrate, bool seekable);
void dlna_finished(uint32_t generation, bool failed);

/* Stop button on the web page */
void dlna_stop(void);

/* Set by the phone: 0..100, and mute */
uint8_t dlna_volume(void);
bool dlna_muted(void);
