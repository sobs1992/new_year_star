#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "settings.h"
#include "storage.h"

#define NVS_NAMESPACE   "star"
#define NVS_KEY         "settings"
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

static const char *TAG = "settings";

/* Single source of truth for validation and for the web UI controls */
typedef struct {
    const char *key;
    int min;
    int max;
} limit_t;

enum { T_REPEAT, T_VOLUME, T_PAUSE };
static const limit_t TRACK_LIMITS[] = {
    [T_REPEAT] = { "repeat", 1, 99 },
    [T_VOLUME] = { "volume", 0, 200 },
    [T_PAUSE] = { "pause_ms", 0, 600000 },
};

static const limit_t LED_LIMITS[] = {
    { "count", 1, SETTINGS_MAX_LEDS },
};

enum { L_MIN, L_MAX, L_ATTACK, L_RELEASE, L_HALFLIFE };
static const limit_t LIGHT_LIMITS[] = {
    [L_MIN] = { "min_brightness", 0, 255 },
    [L_MAX] = { "max_brightness", 0, 255 },
    [L_ATTACK] = { "attack_ms", 1, 1000 },
    [L_RELEASE] = { "release_ms", 10, 5000 },
    [L_HALFLIFE] = { "gain_halflife_ms", 100, 30000 },
};

enum { S_GATE, S_SENSITIVITY, S_COOLDOWN };
static const limit_t SENSOR_LIMITS[] = {
    [S_GATE] = { "max_gate", 2, 8 },
    [S_SENSITIVITY] = { "sensitivity", 0, 100 },
    [S_COOLDOWN] = { "cooldown_s", 0, 600 },
};

enum { R_VOLUME, R_MINUTES };
static const limit_t RADIO_LIMITS[] = {
    [R_VOLUME] = { "volume", 0, 200 },
    [R_MINUTES] = { "sensor_minutes", 1, 240 },
};

static const char *const MODE_NAMES[] = {
    [PLAYER_MODE_PLAYLIST] = "playlist",
    [PLAYER_MODE_RADIO] = "radio",
};

static const char *const COLOR_NAMES[] = {
    [LIGHT_RED] = "red",
    [LIGHT_GREEN] = "green",
    [LIGHT_BLUE] = "blue",
    [LIGHT_RGB] = "rgb",
};

static const char *const TRIGGER_NAMES[] = {
    [SENSOR_TRIGGER_MOTION] = "motion",
    [SENSOR_TRIGGER_PRESENCE] = "presence",
};

static const track_settings_t TRACK_DEFAULTS = {
    .enabled = true,
    .repeat = 1,
    .volume = 50,
    .pause_ms = 1000,
};

#define LIGHT_DEFAULTS { \
    .color = LIGHT_RED, \
    .min_brightness = 16, \
    .max_brightness = 160, \
    .attack_ms = 35, \
    .release_ms = 330, \
    .gain_halflife_ms = 2800, \
}

static const led_settings_t LED_DEFAULTS = {
    .count = 24,
    .light = { [PLAYER_MODE_PLAYLIST] = LIGHT_DEFAULTS, [PLAYER_MODE_RADIO] = LIGHT_DEFAULTS },
};

static const sensor_settings_t SENSOR_DEFAULTS = {
    .enabled = false,
    .trigger = SENSOR_TRIGGER_MOTION,
    .max_gate = 6,
    .sensitivity = 40,
    .cooldown_s = 30,
};

/* Also offered in the web UI to add to an existing list */
static const radio_station_t STATION_PRESETS[] = {
    { "SomaFM Christmas Lounge", "https://ice6.somafm.com/christmas-128-mp3" },
    { "Радио Record", "https://radiorecord.hostingradio.ru/rr_main96.aacp" },
    { "Radio Swiss Jazz", "https://stream.srg-ssr.ch/m/rsj/mp3_128" },
    { "FIP", "https://icecast.radiofrance.fr/fip-midfi.mp3" },
    { "Наше Радио", "https://nashe1.hostingradio.ru/nashe-128.mp3" },
    { "Rock FM", "https://nashe1.hostingradio.ru/rock-128.mp3" },
    { "Радио Ultra", "https://nashe1.hostingradio.ru/ultra-128.mp3" },
    { "Record Rock", "https://radiorecord.hostingradio.ru/rock96.aacp" },
    { "SomaFM Metal Detector", "https://ice6.somafm.com/metal-128-mp3" },
    { "laut.fm Metal", "https://stream.laut.fm/metal" },
    { "181.fm Rock 181", "http://listen.181fm.com/181-rock_128k.mp3" },
};
_Static_assert(ARRAY_SIZE(STATION_PRESETS) <= RADIO_MAX_STATIONS, "too many station presets");

/* .stations are copied from STATION_PRESETS in settings_init() */
static const radio_settings_t RADIO_DEFAULTS = {
    .station_count = ARRAY_SIZE(STATION_PRESETS),
    .current = 0,
    .volume = 50,
    .sensor_minutes = 5,
};

typedef struct {
    char name[TRACK_NAME_SIZE];
    track_settings_t cfg;
} track_entry_t;

typedef struct {
    track_entry_t tracks[SETTINGS_MAX_TRACKS]; /* sorted by name */
    size_t track_count;
    led_settings_t led;
    sensor_settings_t sensor[PLAYER_MODE_COUNT]; /* by player_mode_t */
    player_mode_t mode;
    radio_settings_t radio;
} all_settings_t;

static SemaphoreHandle_t lock;
static all_settings_t cur;
static atomic_uint_fast32_t led_version;
static atomic_uint_fast32_t radar_version;
static char *saved_json; /* last JSON written to NVS, to skip identical writes */
/* Bumped on every change. The web UI sends back the revision it edited, so a page with
 * stale settings cannot overwrite newer ones. Random start: pages open across a reboot
 * are stale too. */
static uint32_t revision;

static int find_track(const all_settings_t *s, const char *name)
{
    for (size_t i = 0; i < s->track_count; i++) {
        if (strcmp(s->tracks[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool read_ints(const cJSON *obj, const limit_t *limits, size_t n, int *out,
                      char *err, size_t err_len)
{
    for (size_t i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, limits[i].key);
        if (!cJSON_IsNumber(item) || item->valuedouble < limits[i].min ||
            item->valuedouble > limits[i].max) {
            snprintf(err, err_len, "%s: expected a number %d..%d",
                     limits[i].key, limits[i].min, limits[i].max);
            return false;
        }
        out[i] = (int)item->valuedouble;
    }
    return true;
}

static bool read_bool(const cJSON *obj, const char *key, bool *out, char *err, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(item)) {
        snprintf(err, err_len, "%s: expected true/false", key);
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

/* Index of the string obj[key] in names, -1 if absent or not a string */
static int read_enum(const cJSON *obj, const char *key, const char *const *names, size_t n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    for (size_t i = 0; cJSON_IsString(item) && i < n; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parse_track(const cJSON *jt, track_settings_t *t, char *err, size_t err_len)
{
    int v[ARRAY_SIZE(TRACK_LIMITS)];
    bool enabled;
    if (!read_bool(jt, "enabled", &enabled, err, err_len) ||
        !read_ints(jt, TRACK_LIMITS, ARRAY_SIZE(TRACK_LIMITS), v, err, err_len)) {
        return false;
    }
    t->enabled = enabled;
    t->repeat = v[T_REPEAT];
    t->volume = v[T_VOLUME];
    t->pause_ms = v[T_PAUSE];
    return true;
}

/* "color" is optional: settings saved before it existed keep the current one */
static bool parse_light(const cJSON *jl, light_settings_t *l, char *err, size_t err_len)
{
    int v[ARRAY_SIZE(LIGHT_LIMITS)];
    if (!read_ints(jl, LIGHT_LIMITS, ARRAY_SIZE(LIGHT_LIMITS), v, err, err_len)) {
        return false;
    }
    if (v[L_MIN] > v[L_MAX]) {
        snprintf(err, err_len, "min_brightness is above max_brightness");
        return false;
    }
    if (cJSON_GetObjectItem(jl, "color")) {
        int color = read_enum(jl, "color", COLOR_NAMES, ARRAY_SIZE(COLOR_NAMES));
        if (color < 0) {
            snprintf(err, err_len, "color: expected red, green, blue or rgb");
            return false;
        }
        l->color = color;
    }
    l->min_brightness = v[L_MIN];
    l->max_brightness = v[L_MAX];
    l->attack_ms = v[L_ATTACK];
    l->release_ms = v[L_RELEASE];
    l->gain_halflife_ms = v[L_HALFLIFE];
    return true;
}

/* {"count": n, "playlist": {light}, "radio": {light}}. Settings saved before the per-mode
 * split have the light fields right in "led": they then apply to both modes. */
static bool parse_led(const cJSON *jl, led_settings_t *l, char *err, size_t err_len)
{
    int count;
    if (!read_ints(jl, LED_LIMITS, ARRAY_SIZE(LED_LIMITS), &count, err, err_len)) {
        return false;
    }
    bool legacy = cJSON_GetObjectItem(jl, "min_brightness") != NULL;
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        const cJSON *jm = cJSON_GetObjectItemCaseSensitive(jl, MODE_NAMES[m]);
        const cJSON *src = cJSON_IsObject(jm) ? jm : legacy ? jl : NULL;
        char why[64];
        if (src && !parse_light(src, &l->light[m], why, sizeof(why))) {
            snprintf(err, err_len, "%s: %s", MODE_NAMES[m], why);
            return false;
        }
    }
    l->count = count;
    return true;
}

static bool parse_sensor(const cJSON *js, sensor_settings_t *s, char *err, size_t err_len)
{
    int v[ARRAY_SIZE(SENSOR_LIMITS)];
    bool enabled;
    if (!read_bool(js, "enabled", &enabled, err, err_len) ||
        !read_ints(js, SENSOR_LIMITS, ARRAY_SIZE(SENSOR_LIMITS), v, err, err_len)) {
        return false;
    }
    int t = read_enum(js, "trigger", TRIGGER_NAMES, ARRAY_SIZE(TRIGGER_NAMES));
    if (t < 0) {
        snprintf(err, err_len, "trigger: expected \"motion\" or \"presence\"");
        return false;
    }
    s->enabled = enabled;
    s->trigger = t;
    s->max_gate = v[S_GATE];
    s->sensitivity = v[S_SENSITIVITY];
    s->cooldown_s = v[S_COOLDOWN];
    return true;
}

/* {"playlist": {sensor}, "radio": {sensor}}. Settings saved before the per-mode split have
 * the fields right in "sensor": they then apply to both modes. */
static bool parse_sensors(const cJSON *js, sensor_settings_t *s, char *err, size_t err_len)
{
    bool legacy = cJSON_GetObjectItem(js, "enabled") != NULL;
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        const cJSON *jm = cJSON_GetObjectItemCaseSensitive(js, MODE_NAMES[m]);
        const cJSON *src = cJSON_IsObject(jm) ? jm : legacy ? js : NULL;
        char why[64];
        if (src && !parse_sensor(src, &s[m], why, sizeof(why))) {
            snprintf(err, err_len, "%s: %s", MODE_NAMES[m], why);
            return false;
        }
    }
    return true;
}

static bool read_string(const cJSON *obj, const char *key, char *out, size_t size,
                        char *err, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0' || strlen(item->valuestring) >= size) {
        snprintf(err, err_len, "%s: expected a string of 1..%u bytes", key, (unsigned)size - 1);
        return false;
    }
    strlcpy(out, item->valuestring, size);
    return true;
}

static bool parse_radio(const cJSON *jr, radio_settings_t *r, char *err, size_t err_len)
{
    int v[ARRAY_SIZE(RADIO_LIMITS)];
    if (!read_ints(jr, RADIO_LIMITS, ARRAY_SIZE(RADIO_LIMITS), v, err, err_len)) {
        return false;
    }
    const cJSON *jstations = cJSON_GetObjectItemCaseSensitive(jr, "stations");
    if (!cJSON_IsArray(jstations) || cJSON_GetArraySize(jstations) > RADIO_MAX_STATIONS) {
        snprintf(err, err_len, "stations: expected up to %d entries", RADIO_MAX_STATIONS);
        return false;
    }
    size_t n = 0;
    const cJSON *js;
    cJSON_ArrayForEach(js, jstations) {
        radio_station_t *st = &r->stations[n++];
        if (!read_string(js, "name", st->name, sizeof(st->name), err, err_len) ||
            !read_string(js, "url", st->url, sizeof(st->url), err, err_len)) {
            return false;
        }
        if (strncmp(st->url, "http://", 7) != 0 && strncmp(st->url, "https://", 8) != 0) {
            snprintf(err, err_len, "%s: url must start with http:// or https://", st->name);
            return false;
        }
    }
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(jr, "current");
    if (!cJSON_IsNumber(current) || current->valueint < -1 || current->valueint >= (int)n) {
        snprintf(err, err_len, "current: expected -1..%d", (int)n - 1);
        return false;
    }
    r->station_count = n;
    r->current = current->valueint;
    r->volume = v[R_VOLUME];
    r->sensor_minutes = v[R_MINUTES];
    return true;
}

/* Parse into the given copy. Tracks are matched by name: unknown names are skipped (file
 * deleted), tracks missing from the JSON keep their values. "sensor", "radio" and "mode" are
 * optional so settings saved before they existed still load. */
static bool parse(const char *json, all_settings_t *s, char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        snprintf(err, err_len, "invalid JSON");
        return false;
    }

    bool ok = false;
    const cJSON *jtracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    const cJSON *jled = cJSON_GetObjectItemCaseSensitive(root, "led");
    const cJSON *jsensor = cJSON_GetObjectItemCaseSensitive(root, "sensor");
    const cJSON *jradio = cJSON_GetObjectItemCaseSensitive(root, "radio");
    const cJSON *jmode = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsArray(jtracks) || !cJSON_IsObject(jled)) {
        snprintf(err, err_len, "expected \"tracks\" array and \"led\" object");
        goto out;
    }

    const cJSON *jt;
    cJSON_ArrayForEach(jt, jtracks) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(jt, "name");
        if (!cJSON_IsString(name)) {
            snprintf(err, err_len, "track without name");
            goto out;
        }
        int idx = find_track(s, name->valuestring);
        if (idx < 0) {
            continue;
        }
        char why[64];
        if (!parse_track(jt, &s->tracks[idx].cfg, why, sizeof(why))) {
            snprintf(err, err_len, "%s: %s", name->valuestring, why);
            goto out;
        }
    }
    if (!parse_led(jled, &s->led, err, err_len)) {
        goto out;
    }
    if (jsensor && !parse_sensors(jsensor, s->sensor, err, err_len)) {
        goto out;
    }
    if (jradio && !parse_radio(jradio, &s->radio, err, err_len)) {
        goto out;
    }
    if (jmode) {
        int mode = read_enum(root, "mode", MODE_NAMES, ARRAY_SIZE(MODE_NAMES));
        if (mode < 0) {
            snprintf(err, err_len, "mode: expected \"playlist\" or \"radio\"");
            goto out;
        }
        s->mode = mode;
    }
    ok = true;

out:
    cJSON_Delete(root);
    return ok;
}

static void save(void)
{
    char *json = settings_to_json(false);
    if (json == NULL) {
        return;
    }
    if (saved_json && strcmp(saved_json, json) == 0) {
        free(json);
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    revision++;
    xSemaphoreGive(lock);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(nvs, NVS_KEY, json, strlen(json) + 1);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Saving to NVS failed: %s", esp_err_to_name(ret));
        free(json);
        return;
    }
    free(saved_json);
    saved_json = json;
}

static char *load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *json = NULL;
    if (nvs_get_blob(nvs, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        json = malloc(len);
        if (json && nvs_get_blob(nvs, NVS_KEY, json, &len) != ESP_OK) {
            free(json);
            json = NULL;
        }
    }
    nvs_close(nvs);
    return json;
}

static bool is_mp3_name(const char *name)
{
    size_t len = strlen(name);
    return name[0] != '.' && len > 4 && len < TRACK_NAME_SIZE && strcasecmp(name + len - 4, ".mp3") == 0;
}

static int compare_tracks(const void *a, const void *b)
{
    return strcmp(((const track_entry_t *)a)->name, ((const track_entry_t *)b)->name);
}

/* Fill cur.tracks from the files in storage, with default settings */
static void scan_tracks(void)
{
    DIR *dir = opendir(STORAGE_BASE_PATH);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", STORAGE_BASE_PATH);
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_type != DT_REG || !is_mp3_name(e->d_name)) {
            continue;
        }
        if (cur.track_count == SETTINGS_MAX_TRACKS) {
            ESP_LOGW(TAG, "More than %d tracks, ignoring %s", SETTINGS_MAX_TRACKS, e->d_name);
            continue;
        }
        track_entry_t *t = &cur.tracks[cur.track_count++];
        strlcpy(t->name, e->d_name, sizeof(t->name));
        t->cfg = TRACK_DEFAULTS;
    }
    closedir(dir);
    qsort(cur.tracks, cur.track_count, sizeof(cur.tracks[0]), compare_tracks);
}

void settings_init(void)
{
    lock = xSemaphoreCreateMutex();
    assert(lock);
    revision = esp_random();

    scan_tracks();
    cur.led = LED_DEFAULTS;
    cur.sensor[PLAYER_MODE_PLAYLIST] = SENSOR_DEFAULTS;
    cur.sensor[PLAYER_MODE_RADIO] = SENSOR_DEFAULTS;
    cur.mode = PLAYER_MODE_PLAYLIST;
    cur.radio = RADIO_DEFAULTS;
    memcpy(cur.radio.stations, STATION_PRESETS, sizeof(STATION_PRESETS));
    for (size_t i = 0; i < cur.track_count; i++) {
        ESP_LOGI(TAG, "Track: %s, %u bytes", cur.tracks[i].name,
                 (unsigned)storage_file_size(cur.tracks[i].name));
    }

    char *json = load();
    if (json == NULL) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        return;
    }
    static all_settings_t s; /* too big for the main task stack */
    s = cur;
    char err[96];
    if (parse(json, &s, err, sizeof(err))) {
        ESP_LOGI(TAG, "Loaded from NVS");
        cur = s;
        saved_json = json;
    } else {
        ESP_LOGW(TAG, "Saved settings rejected (%s), using defaults", err);
        free(json);
    }
}

size_t settings_track_count(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t n = cur.track_count;
    xSemaphoreGive(lock);
    return n;
}

bool settings_get_track(size_t index, char name[TRACK_NAME_SIZE], track_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = index < cur.track_count;
    if (ok) {
        strlcpy(name, cur.tracks[index].name, TRACK_NAME_SIZE);
        *out = cur.tracks[index].cfg;
    }
    xSemaphoreGive(lock);
    return ok;
}

bool settings_find_track(const char *name, track_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = find_track(&cur, name);
    if (idx >= 0) {
        *out = cur.tracks[idx].cfg;
    }
    xSemaphoreGive(lock);
    return idx >= 0;
}

esp_err_t settings_add_track(const char *name)
{
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (find_track(&cur, name) >= 0) {
        /* already listed */
    } else if (cur.track_count == SETTINGS_MAX_TRACKS || strlen(name) >= TRACK_NAME_SIZE) {
        ret = ESP_ERR_NO_MEM;
    } else {
        track_entry_t *t = &cur.tracks[cur.track_count++];
        strlcpy(t->name, name, sizeof(t->name));
        t->cfg = TRACK_DEFAULTS;
        qsort(cur.tracks, cur.track_count, sizeof(cur.tracks[0]), compare_tracks);
    }
    xSemaphoreGive(lock);
    if (ret == ESP_OK) {
        save();
    }
    return ret;
}

void settings_remove_track(const char *name)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = find_track(&cur, name);
    if (idx >= 0) {
        memmove(&cur.tracks[idx], &cur.tracks[idx + 1],
                (cur.track_count - idx - 1) * sizeof(cur.tracks[0]));
        cur.track_count--;
    }
    xSemaphoreGive(lock);
    save();
}

void settings_get_led(led_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cur.led;
    xSemaphoreGive(lock);
}

uint32_t settings_led_version(void)
{
    return atomic_load(&led_version);
}

void settings_get_sensor(sensor_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cur.sensor[cur.mode];
    xSemaphoreGive(lock);
}

uint32_t settings_radar_version(void)
{
    return atomic_load(&radar_version);
}

player_mode_t settings_get_mode(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    player_mode_t mode = cur.mode;
    xSemaphoreGive(lock);
    return mode;
}

bool settings_get_current_station(radio_station_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cur.radio.current >= 0 && cur.radio.current < cur.radio.station_count;
    if (ok) {
        *out = cur.radio.stations[cur.radio.current];
    }
    xSemaphoreGive(lock);
    return ok;
}

uint8_t settings_get_radio_volume(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint8_t volume = cur.radio.volume;
    xSemaphoreGive(lock);
    return volume;
}

uint16_t settings_get_radio_minutes(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint16_t minutes = cur.radio.sensor_minutes;
    xSemaphoreGive(lock);
    return minutes;
}

uint32_t settings_revision(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t rev = revision;
    xSemaphoreGive(lock);
    return rev;
}

bool settings_is_track_name(const char *name)
{
    return is_mp3_name(name);
}

static void add_limits(cJSON *parent, const char *name, const limit_t *limits, size_t n)
{
    cJSON *group = cJSON_AddObjectToObject(parent, name);
    for (size_t i = 0; i < n; i++) {
        int range[2] = { limits[i].min, limits[i].max };
        cJSON_AddItemToObject(group, limits[i].key, cJSON_CreateIntArray(range, 2));
    }
}

char *settings_to_json(bool for_ui)
{
    cJSON *root = cJSON_CreateObject();

    xSemaphoreTake(lock, portMAX_DELAY);
    if (for_ui) {
        cJSON_AddNumberToObject(root, "revision", revision);
    }
    cJSON *jtracks = cJSON_AddArrayToObject(root, "tracks");
    for (size_t i = 0; i < cur.track_count; i++) {
        const track_entry_t *t = &cur.tracks[i];
        cJSON *jt = cJSON_CreateObject();
        cJSON_AddStringToObject(jt, "name", t->name);
        cJSON_AddBoolToObject(jt, "enabled", t->cfg.enabled);
        cJSON_AddNumberToObject(jt, "repeat", t->cfg.repeat);
        cJSON_AddNumberToObject(jt, "volume", t->cfg.volume);
        cJSON_AddNumberToObject(jt, "pause_ms", t->cfg.pause_ms);
        if (for_ui) {
            cJSON_AddNumberToObject(jt, "size", storage_file_size(t->name));
        }
        cJSON_AddItemToArray(jtracks, jt);
    }

    cJSON *jled = cJSON_AddObjectToObject(root, "led");
    cJSON_AddNumberToObject(jled, "count", cur.led.count);
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        const light_settings_t *l = &cur.led.light[m];
        cJSON *jl = cJSON_AddObjectToObject(jled, MODE_NAMES[m]);
        cJSON_AddStringToObject(jl, "color", COLOR_NAMES[l->color]);
        cJSON_AddNumberToObject(jl, "min_brightness", l->min_brightness);
        cJSON_AddNumberToObject(jl, "max_brightness", l->max_brightness);
        cJSON_AddNumberToObject(jl, "attack_ms", l->attack_ms);
        cJSON_AddNumberToObject(jl, "release_ms", l->release_ms);
        cJSON_AddNumberToObject(jl, "gain_halflife_ms", l->gain_halflife_ms);
    }

    cJSON *jsensor = cJSON_AddObjectToObject(root, "sensor");
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        const sensor_settings_t *ss = &cur.sensor[m];
        cJSON *js = cJSON_AddObjectToObject(jsensor, MODE_NAMES[m]);
        cJSON_AddBoolToObject(js, "enabled", ss->enabled);
        cJSON_AddStringToObject(js, "trigger", TRIGGER_NAMES[ss->trigger]);
        cJSON_AddNumberToObject(js, "max_gate", ss->max_gate);
        cJSON_AddNumberToObject(js, "sensitivity", ss->sensitivity);
        cJSON_AddNumberToObject(js, "cooldown_s", ss->cooldown_s);
    }

    cJSON_AddStringToObject(root, "mode", MODE_NAMES[cur.mode]);
    cJSON *jradio = cJSON_AddObjectToObject(root, "radio");
    cJSON *jstations = cJSON_AddArrayToObject(jradio, "stations");
    for (size_t i = 0; i < cur.radio.station_count; i++) {
        cJSON *js = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", cur.radio.stations[i].name);
        cJSON_AddStringToObject(js, "url", cur.radio.stations[i].url);
        cJSON_AddItemToArray(jstations, js);
    }
    cJSON_AddNumberToObject(jradio, "current", cur.radio.current);
    cJSON_AddNumberToObject(jradio, "volume", cur.radio.volume);
    cJSON_AddNumberToObject(jradio, "sensor_minutes", cur.radio.sensor_minutes);
    xSemaphoreGive(lock);

    if (for_ui) {
        cJSON *jlimits = cJSON_AddObjectToObject(root, "limits");
        add_limits(jlimits, "track", TRACK_LIMITS, ARRAY_SIZE(TRACK_LIMITS));
        add_limits(jlimits, "led", LED_LIMITS, ARRAY_SIZE(LED_LIMITS));
        add_limits(jlimits, "light", LIGHT_LIMITS, ARRAY_SIZE(LIGHT_LIMITS));
        add_limits(jlimits, "sensor", SENSOR_LIMITS, ARRAY_SIZE(SENSOR_LIMITS));
        add_limits(jlimits, "radio", RADIO_LIMITS, ARRAY_SIZE(RADIO_LIMITS));
        cJSON_AddNumberToObject(jlimits, "max_stations", RADIO_MAX_STATIONS);
        cJSON_AddNumberToObject(jlimits, "max_station_name_bytes", RADIO_NAME_SIZE - 1);
        cJSON_AddNumberToObject(jlimits, "max_url_bytes", RADIO_URL_SIZE - 1);

        cJSON *jpresets = cJSON_AddArrayToObject(root, "station_presets");
        for (size_t i = 0; i < ARRAY_SIZE(STATION_PRESETS); i++) {
            cJSON *js = cJSON_CreateObject();
            cJSON_AddStringToObject(js, "name", STATION_PRESETS[i].name);
            cJSON_AddStringToObject(js, "url", STATION_PRESETS[i].url);
            cJSON_AddItemToArray(jpresets, js);
        }
        cJSON_AddNumberToObject(jlimits, "max_tracks", SETTINGS_MAX_TRACKS);
        cJSON_AddNumberToObject(jlimits, "max_name_bytes", TRACK_NAME_SIZE - 1);

        size_t total, used;
        storage_info(&total, &used);
        cJSON *jstorage = cJSON_AddObjectToObject(root, "storage");
        cJSON_AddNumberToObject(jstorage, "total", total);
        cJSON_AddNumberToObject(jstorage, "used", used);
        cJSON_AddNumberToObject(jstorage, "upload_max", storage_upload_space());
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

esp_err_t settings_from_json(const char *json, char *err, size_t err_len)
{
    static all_settings_t s; /* only the httpd task calls this */
    xSemaphoreTake(lock, portMAX_DELAY);
    s = cur;
    xSemaphoreGive(lock);

    if (!parse(json, &s, err, err_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    const cJSON *jrev = cJSON_GetObjectItemCaseSensitive(root, "revision");
    double rev = cJSON_IsNumber(jrev) ? jrev->valuedouble : -1;
    cJSON_Delete(root);

    xSemaphoreTake(lock, portMAX_DELAY);
    if (rev != revision) {
        xSemaphoreGive(lock);
        snprintf(err, err_len, "revision: settings were changed elsewhere, reload them");
        return ESP_ERR_INVALID_STATE;
    }
    /* Only settings are taken from s: the track list may have changed meanwhile */
    for (size_t i = 0; i < s.track_count; i++) {
        int idx = find_track(&cur, s.tracks[i].name);
        if (idx >= 0) {
            cur.tracks[idx].cfg = s.tracks[i].cfg;
        }
    }
    /* The light settings in use depend on the mode, so a mode switch counts too */
    bool led_changed = memcmp(&cur.led, &s.led, sizeof(s.led)) != 0 || cur.mode != s.mode;
    /* The radar holds the gate and threshold of the current mode only */
    const sensor_settings_t *radar_old = &cur.sensor[cur.mode], *radar_new = &s.sensor[s.mode];
    bool radar_changed = radar_old->max_gate != radar_new->max_gate ||
                         radar_old->sensitivity != radar_new->sensitivity;
    cur.led = s.led;
    memcpy(cur.sensor, s.sensor, sizeof(cur.sensor));
    cur.mode = s.mode;
    cur.radio = s.radio;
    xSemaphoreGive(lock);
    if (led_changed) {
        atomic_fetch_add(&led_version, 1);
    }
    if (radar_changed) {
        atomic_fetch_add(&radar_version, 1);
    }

    save();
    return ESP_OK;
}
