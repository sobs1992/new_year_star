#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spotify Connect receiver (cspot): the Spotify app finds the star over mDNS and logs it in
 * with the phone's account (Premium only). Audio files are located with a token of a
 * developer app whose Client ID / Secret are in the cast settings. */

/* Register the zeroconf handlers (/spotify_info) and start the session task */
void spotify_start(httpd_handle_t httpd);

/* On / off and the developer app's keys (32 hex digits, or empty); without keys it is not
 * announced. A change logs the session out. May come before spotify_start(). */
void spotify_configure(bool enabled, const char *client_id, const char *client_secret);

/* The phone plays on the star, or paused it a moment ago */
bool spotify_active(void);

/* Up to max_frames stereo 16-bit frames at 44.1 kHz; waits up to timeout_ms for the decoder.
 * Returns 0 while paused or when nothing is decoded yet. */
size_t spotify_read(int16_t *pcm, size_t max_frames, int timeout_ms);

/* Set by the phone: 0..100 */
uint8_t spotify_volume(void);

typedef enum {
    SPOTIFY_OFF,        /* switched off in the settings */
    SPOTIFY_NO_KEYS,    /* Client ID / Secret missing: not announced */
    SPOTIFY_WAITING,    /* announced, nobody logged in */
    SPOTIFY_CONNECTING, /* logging in with the account the phone sent */
    SPOTIFY_CONNECTED,  /* logged in, the star is not playing */
    SPOTIFY_PLAYING,
    SPOTIFY_PAUSED,
} spotify_state_t;

typedef struct {
    spotify_state_t state;
    bool login_failed; /* the last login attempt was declined or could not connect */
    bool key_failed;   /* Spotify rejected the Client ID / Secret, or could not be asked */
    char user[32];
    char title[64];
    char artist[64];
} spotify_status_t;

void spotify_get_status(spotify_status_t *out);

/* Stop button on the web page: pause, also in the phone's app */
void spotify_stop(void);

struct cJSON;
/* "spotify" of /api/status */
void spotify_status_json(struct cJSON *out);

#ifdef __cplusplus
}
#endif
