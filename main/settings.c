#include <ctype.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "app_config.h"
#include "settings.h"
#include "storage.h"

#define NVS_KEY         "settings"
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

static const char *TAG = "settings";

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
    cast_settings_t cast;
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
static void (*on_change)(void);

/* ---------- field tables ---------- */

/* Every setting is described once, here: JSON key, place in its struct, range, default. The
 * tables drive the defaults, the validation, the JSON both ways and the limits the web page
 * builds its controls from. A new field is a struct member plus a line in a table (and its
 * label in index.html). */

typedef enum {
    FIELD_INT,  /* number min..max */
    FIELD_BOOL,
    FIELD_ENUM, /* one of the strings names[0..max], kept as its index */
} field_kind_t;

typedef struct {
    const char *key;
    field_kind_t kind;
    size_t offset;
    size_t size;     /* of the struct member: 1, 2 or 4 bytes, unsigned */
    int min, max;
    int def;
    const char *const *names; /* FIELD_ENUM */
    bool optional;   /* came later: absent from the JSON, it keeps its value */
} field_t;

/* A struct the fields belong to */
typedef struct {
    const field_t *fields;
    size_t count;
    /* Rules across fields, checked after reading them; NULL if none */
    bool (*check)(const void *value, char *err, size_t err_len);
} group_t;

#define GROUP(fields_, check_) { .fields = fields_, .count = ARRAY_SIZE(fields_), .check = check_ }

#define MEMBER_SIZE(type, member) sizeof(((type *)0)->member)
/* Compile error when hi does not fit the (unsigned) member */
#define FITS(type, member, hi) (0 * sizeof(char[(hi) <= (__typeof__(((type *)0)->member))~0ULL ? 1 : -1]))

/* F_INT(struct, member, min, max, default[, .optional = true]) and the like */
#define F_INT(type, member, lo, hi, dflt, ...) { \
    .key = #member, .kind = FIELD_INT, .offset = offsetof(type, member), \
    .size = MEMBER_SIZE(type, member) + FITS(type, member, hi), \
    .min = lo, .max = hi, .def = dflt, __VA_ARGS__ }
#define F_BOOL(type, member, dflt, ...) { \
    .key = #member, .kind = FIELD_BOOL, .offset = offsetof(type, member), \
    .size = MEMBER_SIZE(type, member), .max = 1, .def = dflt, __VA_ARGS__ }
#define F_ENUM(type, member, names_, dflt, ...) { \
    .key = #member, .kind = FIELD_ENUM, .offset = offsetof(type, member), \
    .size = MEMBER_SIZE(type, member), .max = ARRAY_SIZE(names_) - 1, .names = names_, \
    .def = dflt, __VA_ARGS__ }

static const char *const MODE_NAMES[] = {
    [PLAYER_MODE_PLAYLIST] = "playlist",
    [PLAYER_MODE_RADIO] = "radio",
};
_Static_assert(ARRAY_SIZE(MODE_NAMES) == PLAYER_MODE_COUNT, "a name for every player mode");

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

static const field_t TRACK_FIELDS[] = {
    F_BOOL(track_settings_t, enabled, true),
    F_INT(track_settings_t, repeat, 1, 99, 1),
    F_INT(track_settings_t, volume, 0, 200, 50),
    F_INT(track_settings_t, pause_ms, 0, 600000, 1000),
};

static const field_t LED_FIELDS[] = {
    F_INT(led_settings_t, count, 1, SETTINGS_MAX_LEDS, 24),
};

static const field_t LIGHT_FIELDS[] = {
    F_ENUM(light_settings_t, color, COLOR_NAMES, LIGHT_RED, .optional = true),
    F_INT(light_settings_t, min_brightness, 0, 255, 16),
    F_INT(light_settings_t, max_brightness, 0, 255, 160),
    F_INT(light_settings_t, idle_brightness, 0, 255, 0, .optional = true),
    F_INT(light_settings_t, attack_ms, 1, 1000, 35),
    F_INT(light_settings_t, release_ms, 10, 5000, 330),
    F_INT(light_settings_t, gain_halflife_ms, 100, 30000, 2800),
};

static const field_t SENSOR_FIELDS[] = {
    F_BOOL(sensor_settings_t, enabled, false),
    F_ENUM(sensor_settings_t, trigger, TRIGGER_NAMES, SENSOR_TRIGGER_MOTION),
    F_INT(sensor_settings_t, max_gate, 2, 8, 6),
    F_INT(sensor_settings_t, sensitivity, 0, 100, 40),
    F_INT(sensor_settings_t, cooldown_s, 0, 600, 30),
};

/* Plus the stations and the current one, see parse_radio() */
static const field_t RADIO_FIELDS[] = {
    F_INT(radio_settings_t, volume, 0, 200, 50),
    F_INT(radio_settings_t, sensor_minutes, 1, 240, 5),
};

/* Plus the Spotify app keys, see parse_cast() */
static const field_t CAST_FIELDS[] = {
    F_BOOL(cast_settings_t, dlna, true),
    F_BOOL(cast_settings_t, airplay, true),
    F_BOOL(cast_settings_t, spotify, true, .optional = true),
};

static const field_t ROOT_FIELDS[] = {
    F_ENUM(all_settings_t, mode, MODE_NAMES, PLAYER_MODE_PLAYLIST, .optional = true),
};

static bool check_light(const void *value, char *err, size_t err_len)
{
    const light_settings_t *l = value;
    if (l->min_brightness > l->max_brightness) {
        snprintf(err, err_len, "min_brightness is above max_brightness");
        return false;
    }
    return true;
}

static const group_t TRACK_GROUP = GROUP(TRACK_FIELDS, NULL);
static const group_t LED_GROUP = GROUP(LED_FIELDS, NULL);
static const group_t LIGHT_GROUP = GROUP(LIGHT_FIELDS, check_light);
static const group_t SENSOR_GROUP = GROUP(SENSOR_FIELDS, NULL);
static const group_t RADIO_GROUP = GROUP(RADIO_FIELDS, NULL);
static const group_t CAST_GROUP = GROUP(CAST_FIELDS, NULL);
static const group_t ROOT_GROUP = GROUP(ROOT_FIELDS, NULL);

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

/* ---------- reading and writing fields ---------- */

static int field_get(const void *value, const field_t *f)
{
    const uint8_t *p = (const uint8_t *)value + f->offset;
    if (f->size == 1) {
        uint8_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    if (f->size == 2) {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void field_set(void *value, const field_t *f, int v)
{
    uint8_t *p = (uint8_t *)value + f->offset;
    if (f->size == 1) {
        uint8_t x = v;
        memcpy(p, &x, sizeof(x));
    } else if (f->size == 2) {
        uint16_t x = v;
        memcpy(p, &x, sizeof(x));
    } else {
        uint32_t x = v;
        memcpy(p, &x, sizeof(x));
    }
}

/* The value of one JSON item; on failure err says what the key expects */
static bool read_field(const cJSON *item, const field_t *f, int *out, char *err, size_t err_len)
{
    switch (f->kind) {
    case FIELD_BOOL:
        if (cJSON_IsBool(item)) {
            *out = cJSON_IsTrue(item);
            return true;
        }
        snprintf(err, err_len, "%s: expected true/false", f->key);
        return false;
    case FIELD_ENUM: {
        for (int i = 0; cJSON_IsString(item) && i <= f->max; i++) {
            if (strcmp(item->valuestring, f->names[i]) == 0) {
                *out = i;
                return true;
            }
        }
        /* "color: expected red, green, blue or rgb" */
        size_t len = snprintf(err, err_len, "%s: expected", f->key);
        for (int i = 0; i <= f->max && len < err_len; i++) {
            len += snprintf(err + len, err_len - len, "%s %s", i == 0 ? "" : i == f->max ? " or" : ",",
                            f->names[i]);
        }
        return false;
    }
    default:
        if (cJSON_IsNumber(item) && item->valuedouble >= f->min && item->valuedouble <= f->max) {
            *out = (int)item->valuedouble;
            return true;
        }
        snprintf(err, err_len, "%s: expected a number %d..%d", f->key, f->min, f->max);
        return false;
    }
}

/* The fields of a group from obj into value, a struct of the group */
static bool read_group(const cJSON *obj, const group_t *g, void *value, char *err, size_t err_len)
{
    for (size_t i = 0; i < g->count; i++) {
        const field_t *f = &g->fields[i];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, f->key);
        int v;
        if (item == NULL && f->optional) {
            continue;
        }
        if (!read_field(item, f, &v, err, err_len)) {
            return false;
        }
        field_set(value, f, v);
    }
    return g->check == NULL || g->check(value, err, err_len);
}

static void write_group(cJSON *obj, const group_t *g, const void *value)
{
    for (size_t i = 0; i < g->count; i++) {
        const field_t *f = &g->fields[i];
        int v = field_get(value, f);
        if (f->kind == FIELD_BOOL) {
            cJSON_AddBoolToObject(obj, f->key, v);
        } else if (f->kind == FIELD_ENUM) {
            cJSON_AddStringToObject(obj, f->key, f->names[v]);
        } else {
            cJSON_AddNumberToObject(obj, f->key, v);
        }
    }
}

static void set_defaults(const group_t *g, void *value)
{
    for (size_t i = 0; i < g->count; i++) {
        field_set(value, &g->fields[i], g->fields[i].def);
    }
}

/* {"key": [min, max]} of the numbers, for the web page controls */
static void add_limits(cJSON *parent, const char *name, const group_t *g)
{
    cJSON *jg = cJSON_AddObjectToObject(parent, name);
    for (size_t i = 0; i < g->count; i++) {
        const field_t *f = &g->fields[i];
        if (f->kind == FIELD_INT) {
            int range[2] = { f->min, f->max };
            cJSON_AddItemToObject(jg, f->key, cJSON_CreateIntArray(range, 2));
        }
    }
}

/* {"playlist": {...}, "radio": {...}}: a struct per player mode, stride bytes apart. Settings
 * saved before the per-mode split have the fields right in obj (it has legacy_key): they then
 * apply to both modes. A mode missing from obj keeps its values. */
static bool read_per_mode(const cJSON *obj, const char *legacy_key, const group_t *g, void *values,
                          size_t stride, char *err, size_t err_len)
{
    bool legacy = cJSON_GetObjectItem(obj, legacy_key) != NULL;
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        const cJSON *jm = cJSON_GetObjectItemCaseSensitive(obj, MODE_NAMES[m]);
        const cJSON *src = cJSON_IsObject(jm) ? jm : legacy ? obj : NULL;
        char why[64];
        if (src && !read_group(src, g, (uint8_t *)values + m * stride, why, sizeof(why))) {
            snprintf(err, err_len, "%s: %s", MODE_NAMES[m], why);
            return false;
        }
    }
    return true;
}

static void write_per_mode(cJSON *obj, const group_t *g, const void *values, size_t stride)
{
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        write_group(cJSON_AddObjectToObject(obj, MODE_NAMES[m]), g, (const uint8_t *)values + m * stride);
    }
}

/* ---------- the parts the tables do not cover ---------- */

static int find_track(const all_settings_t *s, const char *name)
{
    for (size_t i = 0; i < s->track_count; i++) {
        if (strcmp(s->tracks[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
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

/* Optional Spotify app key: absent keeps the current one, otherwise empty or 32 hex digits */
static bool read_spotify_key(const cJSON *obj, const char *key, char out[SPOTIFY_KEY_SIZE],
                             char *err, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL) {
        return true;
    }
    const char *v = cJSON_GetStringValue(item);
    bool ok = v && (v[0] == '\0' || strlen(v) == SPOTIFY_KEY_SIZE - 1);
    for (const char *c = v; ok && *c; c++) {
        ok = isxdigit((unsigned char)*c);
    }
    if (!ok) {
        snprintf(err, err_len, "%s: expected 32 hex digits or empty", key);
        return false;
    }
    strlcpy(out, v, SPOTIFY_KEY_SIZE);
    return true;
}

static bool parse_cast(const cJSON *jc, cast_settings_t *c, char *err, size_t err_len)
{
    return read_group(jc, &CAST_GROUP, c, err, err_len) &&
           read_spotify_key(jc, "spotify_client_id", c->spotify_client_id, err, err_len) &&
           read_spotify_key(jc, "spotify_client_secret", c->spotify_client_secret, err, err_len);
}

static bool parse_radio(const cJSON *jr, radio_settings_t *r, char *err, size_t err_len)
{
    if (!read_group(jr, &RADIO_GROUP, r, err, err_len)) {
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
    return true;
}

/* Parse into the given copy. Tracks are matched by name: unknown names are skipped (file
 * deleted), tracks missing from the JSON keep their values. "sensor", "radio", "mode" and
 * "cast" are optional so settings saved before they existed still load. */
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
    const cJSON *jcast = cJSON_GetObjectItemCaseSensitive(root, "cast");
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
        if (!read_group(jt, &TRACK_GROUP, &s->tracks[idx].cfg, why, sizeof(why))) {
            snprintf(err, err_len, "%s: %s", name->valuestring, why);
            goto out;
        }
    }
    ok = read_group(jled, &LED_GROUP, &s->led, err, err_len) &&
         read_per_mode(jled, "min_brightness", &LIGHT_GROUP, s->led.light, sizeof(s->led.light[0]),
                       err, err_len) &&
         (!jsensor || read_per_mode(jsensor, "enabled", &SENSOR_GROUP, s->sensor, sizeof(s->sensor[0]),
                                    err, err_len)) &&
         (!jradio || parse_radio(jradio, &s->radio, err, err_len)) &&
         (!jcast || parse_cast(jcast, &s->cast, err, err_len)) &&
         read_group(root, &ROOT_GROUP, s, err, err_len);

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

static int compare_tracks(const void *a, const void *b)
{
    return strcmp(((const track_entry_t *)a)->name, ((const track_entry_t *)b)->name);
}

/* A file found in storage: into cur.tracks with default settings */
static void add_found_track(const char *name, void *ctx)
{
    if (cur.track_count == SETTINGS_MAX_TRACKS) {
        ESP_LOGW(TAG, "More than %d tracks, ignoring %s", SETTINGS_MAX_TRACKS, name);
        return;
    }
    track_entry_t *t = &cur.tracks[cur.track_count++];
    strlcpy(t->name, name, sizeof(t->name));
    set_defaults(&TRACK_GROUP, &t->cfg);
}

void settings_init(void)
{
    lock = xSemaphoreCreateMutex();
    assert(lock);
    revision = esp_random();

    storage_for_each_track(add_found_track, NULL);
    qsort(cur.tracks, cur.track_count, sizeof(cur.tracks[0]), compare_tracks);
    set_defaults(&LED_GROUP, &cur.led);
    for (int m = 0; m < PLAYER_MODE_COUNT; m++) {
        set_defaults(&LIGHT_GROUP, &cur.led.light[m]);
        set_defaults(&SENSOR_GROUP, &cur.sensor[m]);
    }
    set_defaults(&ROOT_GROUP, &cur);
    set_defaults(&RADIO_GROUP, &cur.radio);
    memcpy(cur.radio.stations, STATION_PRESETS, sizeof(STATION_PRESETS));
    cur.radio.station_count = ARRAY_SIZE(STATION_PRESETS);
    cur.radio.current = 0;
    set_defaults(&CAST_GROUP, &cur.cast);
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
        set_defaults(&TRACK_GROUP, &t->cfg);
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

void settings_get_cast(cast_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cur.cast;
    xSemaphoreGive(lock);
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
        write_group(jt, &TRACK_GROUP, &t->cfg);
        if (for_ui) {
            cJSON_AddNumberToObject(jt, "size", storage_file_size(t->name));
        }
        cJSON_AddItemToArray(jtracks, jt);
    }

    cJSON *jled = cJSON_AddObjectToObject(root, "led");
    write_group(jled, &LED_GROUP, &cur.led);
    write_per_mode(jled, &LIGHT_GROUP, cur.led.light, sizeof(cur.led.light[0]));
    write_per_mode(cJSON_AddObjectToObject(root, "sensor"), &SENSOR_GROUP, cur.sensor, sizeof(cur.sensor[0]));
    write_group(root, &ROOT_GROUP, &cur);

    cJSON *jradio = cJSON_AddObjectToObject(root, "radio");
    cJSON *jstations = cJSON_AddArrayToObject(jradio, "stations");
    for (size_t i = 0; i < cur.radio.station_count; i++) {
        cJSON *js = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", cur.radio.stations[i].name);
        cJSON_AddStringToObject(js, "url", cur.radio.stations[i].url);
        cJSON_AddItemToArray(jstations, js);
    }
    cJSON_AddNumberToObject(jradio, "current", cur.radio.current);
    write_group(jradio, &RADIO_GROUP, &cur.radio);

    cJSON *jcast = cJSON_AddObjectToObject(root, "cast");
    write_group(jcast, &CAST_GROUP, &cur.cast);
    cJSON_AddStringToObject(jcast, "spotify_client_id", cur.cast.spotify_client_id);
    if (for_ui) {
        /* The page only learns whether it is there; it sends a new one to replace it */
        cJSON_AddBoolToObject(jcast, "spotify_secret_set", cur.cast.spotify_client_secret[0] != '\0');
    } else {
        cJSON_AddStringToObject(jcast, "spotify_client_secret", cur.cast.spotify_client_secret);
    }
    xSemaphoreGive(lock);

    if (for_ui) {
        cJSON *jlimits = cJSON_AddObjectToObject(root, "limits");
        add_limits(jlimits, "track", &TRACK_GROUP);
        add_limits(jlimits, "led", &LED_GROUP);
        add_limits(jlimits, "light", &LIGHT_GROUP);
        add_limits(jlimits, "sensor", &SENSOR_GROUP);
        add_limits(jlimits, "radio", &RADIO_GROUP);
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
    cur.cast = s.cast;
    xSemaphoreGive(lock);
    if (led_changed) {
        atomic_fetch_add(&led_version, 1);
    }
    if (radar_changed) {
        atomic_fetch_add(&radar_version, 1);
    }

    save();
    if (on_change) {
        on_change();
    }
    return ESP_OK;
}

void settings_on_change(void (*changed)(void))
{
    on_change = changed;
}
