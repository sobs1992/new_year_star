#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "radio.h"

/* DLNA / UPnP media renderer: phones find the star over SSDP (home network and the star's
 * own network) and send it the URL of a file to play. */

typedef struct {
    uint32_t generation; /* changes with every new URL, and on Play after Stop */
    bool paused;
    char url[STREAM_URL_SIZE];
    char mime[48];       /* from the DIDL-Lite protocolInfo, may be empty */
    char title[64];
} dlna_job_t;

/* Register the description / control / event handlers and start SSDP */
void dlna_start(httpd_handle_t httpd);

/* What the phone wants played: true while playing or paused. out may be NULL. */
bool dlna_get_job(dlna_job_t *out);

/* From the player: progress, and the end of a job (failed: could not be played) */
void dlna_set_position(uint32_t generation, uint32_t ms);
void dlna_finished(uint32_t generation, bool failed);

/* Stop button on the web page */
void dlna_stop(void);

/* Set by the phone: 0..100, and mute */
uint8_t dlna_volume(void);
bool dlna_muted(void);
