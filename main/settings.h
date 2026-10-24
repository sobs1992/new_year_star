#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SETTINGS_MAX_TRACKS 32
#define SETTINGS_MAX_LEDS   300
#define TRACK_NAME_SIZE     64  /* bytes incl. '\0', LittleFS name limit */

typedef struct {
    bool enabled;
    uint8_t repeat;    /* times in a row */
    uint8_t volume;    /* percent; above 100 clips on loud peaks */
    uint32_t pause_ms; /* silence after the last repeat */
} track_settings_t;

typedef enum {
    PLAYER_MODE_PLAYLIST,
    PLAYER_MODE_RADIO,
    PLAYER_MODE_COUNT,
} player_mode_t;

typedef enum {
    LIGHT_RED,   /* one channel follows the loudness */
    LIGHT_GREEN,
    LIGHT_BLUE,
    LIGHT_RGB,   /* bass -> red, mids -> green, highs -> blue */
} light_color_t;

/* Light music, separate for each player mode */
typedef struct {
    uint8_t color;             /* light_color_t */
    uint8_t min_brightness;    /* background glow, also during pauses */
    uint8_t max_brightness;
    uint16_t attack_ms;        /* rise time constant */
    uint16_t release_ms;       /* fall time constant */
    uint16_t gain_halflife_ms; /* auto gain: how fast the peak level is forgotten */
} light_settings_t;

typedef struct {
    uint16_t count;
    light_settings_t light[PLAYER_MODE_COUNT]; /* by player_mode_t */
} led_settings_t;

typedef enum {
    SENSOR_TRIGGER_MOTION,   /* moving target only */
    SENSOR_TRIGGER_PRESENCE, /* moving or stationary target */
} sensor_trigger_t;

typedef struct {
    bool enabled;         /* false: playlist loops without waiting */
    uint8_t trigger;      /* sensor_trigger_t */
    uint8_t max_gate;     /* farthest radar gate, 0.75 m each */
    uint8_t sensitivity;  /* radar energy threshold 0..100, lower is more sensitive */
    uint16_t cooldown_s;  /* from the end of a cycle to the next trigger */
} sensor_settings_t;

#define RADIO_MAX_STATIONS  16
#define RADIO_NAME_SIZE     48
#define RADIO_URL_SIZE      256

typedef struct {
    char name[RADIO_NAME_SIZE];
    char url[RADIO_URL_SIZE]; /* http:// or https://, MP3/AAC stream or .m3u/.pls */
} radio_station_t;

typedef struct {
    radio_station_t stations[RADIO_MAX_STATIONS];
    uint8_t station_count;
    int8_t current;          /* index into stations, -1 when none */
    uint8_t volume;          /* percent */
    uint16_t sensor_minutes; /* how long to play after a sensor trigger */
} radio_settings_t;

/* Scan the MP3 files in storage, then load settings from NVS or fall back to defaults.
 * Needs nvs_flash_init and storage_init done. */
void settings_init(void);

/* Tracks are the MP3 files in storage, sorted by name; settings are kept per file name */
size_t settings_track_count(void);
/* false when index is past the end (the list may shrink while iterating) */
bool settings_get_track(size_t index, char name[TRACK_NAME_SIZE], track_settings_t *out);
bool settings_find_track(const char *name, track_settings_t *out);
/* A file was added to / removed from storage */
esp_err_t settings_add_track(const char *name);
void settings_remove_track(const char *name);
/* Acceptable track file name: "*.mp3", not hidden, fits TRACK_NAME_SIZE */
bool settings_is_track_name(const char *name);

void settings_get_led(led_settings_t *out);
/* Changes on every LED settings or player mode update, to re-read them cheaply */
uint32_t settings_led_version(void);

/* Sensor settings of the current player mode: each mode has its own */
void settings_get_sensor(sensor_settings_t *out);
/* Changes only when parameters stored in the radar itself change (gate, sensitivity),
 * also by a mode switch to different ones */
uint32_t settings_radar_version(void);

player_mode_t settings_get_mode(void);
/* false when no station is selected */
bool settings_get_current_station(radio_station_t *out);
uint8_t settings_get_radio_volume(void);
uint16_t settings_get_radio_minutes(void);

/* JSON of all settings; for_ui adds value limits, file sizes and storage usage. Caller frees. */
char *settings_to_json(bool for_ui);
/* Validate the whole document, apply and save to NVS; err gets a reason on failure.
 * The document must carry the current "revision" (from settings_to_json(true)),
 * otherwise ESP_ERR_INVALID_STATE: it was edited from outdated settings. */
esp_err_t settings_from_json(const char *json, char *err, size_t err_len);
uint32_t settings_revision(void);
