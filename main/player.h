#pragma once

#include <stdbool.h>
#include "settings.h"

typedef enum {
    PLAYER_IDLE,    /* no track enabled */
    PLAYER_WAITING, /* waiting for the motion sensor */
    PLAYER_PLAYING,
    PLAYER_PAUSE,   /* pause after a track */
    PLAYER_RADIO,   /* track holds the station name */
    PLAYER_STOPPED, /* by the stop button */
    PLAYER_CAST,    /* a phone casts (DLNA / AirPlay / Spotify); track holds what */
} player_status_t;

/* Start the task that plays the enabled tracks or the radio: continuously, or per sensor trigger */
void player_start(void);

/* Start button: play now (with the sensor: one cycle without waiting for it) */
void player_play(void);
/* Stop button: silence right away (casting too), ignore the sensor until player_play() */
void player_stop(void);

/* track is empty when not applicable */
void player_get_status(player_status_t *status, char track[TRACK_NAME_SIZE]);

/* The file is open by the player right now */
bool player_is_playing(const char *name);
