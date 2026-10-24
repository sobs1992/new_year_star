/* settings.c with storage.c: defaults, loading what earlier firmware saved, validation,
 * revisions, the track list */

#include "cJSON.h"
#include "settings.h"
#include "stubs.h"
#include "test.h"

#define NVS_NS  "star"
#define NVS_KEY "settings"

#define DEFAULT_TRACK(name) \
    "{\"name\":\"" name "\",\"enabled\":true,\"repeat\":1,\"volume\":50,\"pause_ms\":1000}"
#define DEFAULT_LIGHT \
    "{\"color\":\"red\",\"min_brightness\":16,\"max_brightness\":160,\"idle_brightness\":0," \
    "\"attack_ms\":35,\"release_ms\":330,\"gain_halflife_ms\":2800}"
#define DEFAULT_SENSOR \
    "{\"enabled\":false,\"trigger\":\"motion\",\"max_gate\":6,\"sensitivity\":40,\"cooldown_s\":30}"
#define DEFAULT_CAST \
    "{\"dlna\":true,\"airplay\":true,\"spotify\":true,\"spotify_client_id\":\"\",\"spotify_client_secret\":\"\"}"

#define CLIENT_ID     "0123456789abcdef0123456789abcdef"
#define CLIENT_SECRET "fedcba9876543210fedcba9876543210"

/* What the current firmware saves, every value off its default */
static const char FULL[] =
    "{\"tracks\":["
        "{\"name\":\"a.mp3\",\"enabled\":false,\"repeat\":3,\"volume\":120,\"pause_ms\":5000},"
        "{\"name\":\"b.mp3\",\"enabled\":true,\"repeat\":2,\"volume\":10,\"pause_ms\":600000}],"
    "\"led\":{\"count\":100,"
        "\"playlist\":{\"color\":\"rgb\",\"min_brightness\":1,\"max_brightness\":2,\"idle_brightness\":3,"
            "\"attack_ms\":4,\"release_ms\":50,\"gain_halflife_ms\":600},"
        "\"radio\":{\"color\":\"blue\",\"min_brightness\":7,\"max_brightness\":8,\"idle_brightness\":9,"
            "\"attack_ms\":10,\"release_ms\":110,\"gain_halflife_ms\":1200}},"
    "\"sensor\":{"
        "\"playlist\":{\"enabled\":true,\"trigger\":\"presence\",\"max_gate\":3,\"sensitivity\":10,\"cooldown_s\":20},"
        "\"radio\":{\"enabled\":false,\"trigger\":\"motion\",\"max_gate\":8,\"sensitivity\":90,\"cooldown_s\":600}},"
    "\"mode\":\"radio\","
    "\"playlist\":{\"sensor_cycles\":4},"
    "\"radio\":{\"stations\":["
        "{\"name\":\"One\",\"url\":\"http://one.example/stream\"},"
        "{\"name\":\"Two\",\"url\":\"https://two.example/list.m3u\"}],"
        "\"current\":1,\"volume\":150,\"sensor_minutes\":30},"
    "\"cast\":{\"dlna\":false,\"airplay\":false,\"spotify\":false,"
        "\"spotify_client_id\":\"" CLIENT_ID "\",\"spotify_client_secret\":\"" CLIENT_SECRET "\"}}";

/* ---------- helpers ---------- */

static const char *const FILES[] = { "b.mp3", "a.mp3" };

/* Start the module with tracks a.mp3 and b.mp3, and with saved settings unless NULL */
static void boot(const char *saved)
{
    stub_storage_files(FILES, sizeof(FILES) / sizeof(FILES[0]));
    if (saved) {
        stub_nvs_put(NVS_NS, NVS_KEY, saved);
    }
    settings_init();
}

static cJSON *current(bool for_ui)
{
    char *json = settings_to_json(for_ui);
    cJSON *root = cJSON_Parse(json);
    free(json);
    CHECK(root != NULL);
    return root;
}

/* Item at a dotted path, array elements by index: "led.radio.color", "tracks.1.name" */
static cJSON *at(cJSON *root, const char *path)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", path);
    cJSON *item = root;
    char *save;
    for (char *seg = strtok_r(buf, ".", &save); seg && item; seg = strtok_r(NULL, ".", &save)) {
        item = cJSON_IsArray(item) ? cJSON_GetArrayItem(item, atoi(seg))
                                   : cJSON_GetObjectItemCaseSensitive(item, seg);
    }
    return item;
}

/* Set the item at path to a JSON value, or delete it when json is NULL */
static void set(cJSON *root, const char *path, const char *json)
{
    char parent_path[128];
    snprintf(parent_path, sizeof(parent_path), "%s", path);
    char *dot = strrchr(parent_path, '.');
    const char *last = dot ? dot + 1 : parent_path;
    cJSON *parent = root;
    if (dot) {
        *dot = '\0';
        parent = at(root, parent_path);
    }
    CHECK(parent != NULL);
    cJSON *value = json ? cJSON_Parse(json) : NULL;
    CHECK(json == NULL || value != NULL);
    if (cJSON_IsArray(parent)) {
        if (value) {
            cJSON_ReplaceItemInArray(parent, atoi(last), value);
        } else {
            cJSON_DeleteItemFromArray(parent, atoi(last));
        }
    } else if (value == NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, last);
    } else if (cJSON_GetObjectItemCaseSensitive(parent, last)) {
        cJSON_ReplaceItemInObjectCaseSensitive(parent, last, value);
    } else {
        cJSON_AddItemToObject(parent, last, value);
    }
}

static bool same_json(const cJSON *a, const cJSON *b)
{
    return a && b && cJSON_Compare(a, b, true);
}

static void check_json(const char *file, int line, cJSON *root, const char *path, const char *expected)
{
    cJSON *want = cJSON_Parse(expected);
    cJSON *got = path ? at(root, path) : root;
    if (want == NULL || !same_json(got, want)) {
        char *text = got ? cJSON_PrintUnformatted(got) : NULL;
        fprintf(stderr, "    %s:%d: %s is %s\n      expected %s\n", file, line, path ? path : "document",
                text ? text : "missing", expected);
        exit(1);
    }
    cJSON_Delete(want);
}

/* The JSON at path (NULL: the whole document) equals expected, keys in any order */
#define CHECK_JSON(root, path, expected) check_json(__FILE__, __LINE__, root, path, expected)

static esp_err_t post(cJSON *doc, char *err, size_t err_len)
{
    char *json = cJSON_PrintUnformatted(doc);
    err[0] = '\0';
    esp_err_t ret = settings_from_json(json, err, err_len);
    free(json);
    return ret;
}

/* The settings as the web page has them right now, with one change applied */
static esp_err_t post_change(const char *path, const char *json)
{
    cJSON *doc = current(true);
    set(doc, path, json);
    char err[96];
    esp_err_t ret = post(doc, err, sizeof(err));
    cJSON_Delete(doc);
    return ret;
}

/* ---------- defaults and loading ---------- */

static void defaults_without_saved_settings(void)
{
    static const char *const files[] = {
        "b.mp3", "a.mp3", "c.MP3",
        ".upload", ".hidden.mp3", ".mp3", "notes.txt", "dir.mp3/", /* not tracks */
        "0123456789012345678901234567890123456789012345678901234567890123.mp3", /* name too long */
    };
    stub_storage_files(files, sizeof(files) / sizeof(files[0]));
    settings_init();

    cJSON *s = current(false);
    CHECK_JSON(s, "tracks", "[" DEFAULT_TRACK("a.mp3") "," DEFAULT_TRACK("b.mp3") "," DEFAULT_TRACK("c.MP3") "]");
    CHECK_JSON(s, "led", "{\"count\":24,\"playlist\":" DEFAULT_LIGHT ",\"radio\":" DEFAULT_LIGHT "}");
    CHECK_JSON(s, "sensor", "{\"playlist\":" DEFAULT_SENSOR ",\"radio\":" DEFAULT_SENSOR "}");
    CHECK_JSON(s, "mode", "\"playlist\"");
    CHECK_JSON(s, "playlist", "{\"sensor_cycles\":1}");
    CHECK_EQ_INT(cJSON_GetArraySize(at(s, "radio.stations")), 11);
    CHECK_JSON(s, "radio.stations.0",
               "{\"name\":\"SomaFM Christmas Lounge\",\"url\":\"https://ice6.somafm.com/christmas-128-mp3\"}");
    CHECK_JSON(s, "radio.current", "0");
    CHECK_JSON(s, "radio.volume", "50");
    CHECK_JSON(s, "radio.sensor_minutes", "5");
    CHECK_JSON(s, "cast", DEFAULT_CAST);
    CHECK_EQ_INT(cJSON_GetArraySize(s), 7);
    CHECK_EQ_INT(stub_nvs_writes, 0);
    CHECK_EQ_INT(settings_track_count(), 3);
}

static void saved_settings_load_unchanged(void)
{
    boot(FULL);
    CHECK_JSON(current(false), NULL, FULL);
    CHECK_EQ_INT(stub_nvs_writes, 0);

    /* The getters the other modules use */
    CHECK_EQ_INT(settings_get_mode(), PLAYER_MODE_RADIO);
    sensor_settings_t ss;
    settings_get_sensor(&ss); /* of the current mode */
    CHECK_EQ_INT(ss.max_gate, 8);
    CHECK_EQ_INT(ss.cooldown_s, 600);
    radio_station_t st;
    CHECK(settings_get_current_station(&st));
    CHECK_EQ_STR(st.name, "Two");
    CHECK_EQ_INT(settings_get_radio_volume(), 150);
    CHECK_EQ_INT(settings_get_radio_minutes(), 30);
    CHECK_EQ_INT(settings_get_playlist_cycles(), 4);
    led_settings_t led;
    settings_get_led(&led);
    CHECK_EQ_INT(led.count, 100);
    CHECK_EQ_INT(led.light[PLAYER_MODE_PLAYLIST].color, LIGHT_RGB);
    CHECK_EQ_INT(led.light[PLAYER_MODE_RADIO].idle_brightness, 9);
    CHECK_EQ_INT(led.light[PLAYER_MODE_RADIO].gain_halflife_ms, 1200);
    cast_settings_t cast;
    settings_get_cast(&cast);
    CHECK(!cast.dlna && !cast.airplay && !cast.spotify);
    CHECK_EQ_STR(cast.spotify_client_secret, CLIENT_SECRET);
    track_settings_t ts;
    CHECK(settings_find_track("a.mp3", &ts));
    CHECK(!ts.enabled);
    CHECK_EQ_INT(ts.repeat, 3);
    CHECK_EQ_INT(ts.volume, 120);
    CHECK_EQ_INT(ts.pause_ms, 5000);
    CHECK(settings_find_track("b.mp3", &ts));
    CHECK_EQ_INT(ts.pause_ms, 600000); /* needs all 32 bits */
    char name[TRACK_NAME_SIZE];
    CHECK(settings_get_track(1, name, &ts));
    CHECK_EQ_STR(name, "b.mp3");
    CHECK(!settings_get_track(2, name, &ts));
}

/* Before the per-mode split the light fields were right in "led", without color and idle */
static void legacy_flat_led_applies_to_both_modes(void)
{
    boot("{\"tracks\":[],\"led\":{\"count\":10,\"min_brightness\":5,\"max_brightness\":200,"
         "\"attack_ms\":20,\"release_ms\":100,\"gain_halflife_ms\":1000}}");
    cJSON *s = current(false);
    const char *light = "{\"color\":\"red\",\"min_brightness\":5,\"max_brightness\":200,\"idle_brightness\":0,"
                        "\"attack_ms\":20,\"release_ms\":100,\"gain_halflife_ms\":1000}";
    CHECK_JSON(s, "led.count", "10");
    CHECK_JSON(s, "led.playlist", light);
    CHECK_JSON(s, "led.radio", light);
    /* Sections that did not exist yet */
    CHECK_JSON(s, "sensor", "{\"playlist\":" DEFAULT_SENSOR ",\"radio\":" DEFAULT_SENSOR "}");
    CHECK_JSON(s, "mode", "\"playlist\"");
    CHECK_JSON(s, "playlist", "{\"sensor_cycles\":1}");
    CHECK_EQ_INT(cJSON_GetArraySize(at(s, "radio.stations")), 11);
    CHECK_JSON(s, "cast", DEFAULT_CAST);
    CHECK_JSON(s, "tracks", "[" DEFAULT_TRACK("a.mp3") "," DEFAULT_TRACK("b.mp3") "]");
}

static void legacy_flat_sensor_applies_to_both_modes(void)
{
    boot("{\"tracks\":[],\"led\":{\"count\":24},\"sensor\":{\"enabled\":true,\"trigger\":\"presence\","
         "\"max_gate\":4,\"sensitivity\":30,\"cooldown_s\":60}}");
    cJSON *s = current(false);
    const char *sensor = "{\"enabled\":true,\"trigger\":\"presence\",\"max_gate\":4,\"sensitivity\":30,\"cooldown_s\":60}";
    CHECK_JSON(s, "sensor.playlist", sensor);
    CHECK_JSON(s, "sensor.radio", sensor);
    CHECK_JSON(s, "led", "{\"count\":24,\"playlist\":" DEFAULT_LIGHT ",\"radio\":" DEFAULT_LIGHT "}");
}

/* Spotify and its keys came later than DLNA and AirPlay */
static void legacy_cast_without_spotify(void)
{
    boot("{\"tracks\":[],\"led\":{\"count\":24},\"cast\":{\"dlna\":false,\"airplay\":true}}");
    CHECK_JSON(current(false), "cast",
               "{\"dlna\":false,\"airplay\":true,\"spotify\":true,\"spotify_client_id\":\"\",\"spotify_client_secret\":\"\"}");
}

/* "color" and "idle_brightness" came later: absent, they keep their values; so does a mode
 * object that is missing altogether */
static void light_fields_that_came_later_are_optional(void)
{
    boot("{\"tracks\":[],\"led\":{\"count\":24,\"playlist\":{\"min_brightness\":5,\"max_brightness\":200,"
         "\"attack_ms\":20,\"release_ms\":100,\"gain_halflife_ms\":1000}}}");
    cJSON *s = current(false);
    CHECK_JSON(s, "led.playlist.color", "\"red\"");
    CHECK_JSON(s, "led.playlist.idle_brightness", "0");
    CHECK_JSON(s, "led.playlist.min_brightness", "5");
    CHECK_JSON(s, "led.radio", DEFAULT_LIGHT);

    CHECK_EQ_INT(post_change("led.radio.color", "\"green\""), ESP_OK);
    CHECK_EQ_INT(post_change("led.radio.color", NULL), ESP_OK);
    CHECK_JSON(current(false), "led.radio.color", "\"green\"");
}

/* Settings are kept per file name: a deleted file is skipped, a new one gets defaults */
static void tracks_are_matched_by_name(void)
{
    boot("{\"tracks\":[{\"name\":\"gone.mp3\",\"enabled\":false,\"repeat\":9,\"volume\":9,\"pause_ms\":9},"
         "{\"name\":\"b.mp3\",\"enabled\":false,\"repeat\":5,\"volume\":7,\"pause_ms\":9}],\"led\":{\"count\":24}}");
    CHECK_JSON(current(false), "tracks",
               "[" DEFAULT_TRACK("a.mp3") ",{\"name\":\"b.mp3\",\"enabled\":false,\"repeat\":5,\"volume\":7,\"pause_ms\":9}]");
}

static void invalid_saved_settings_fall_back_to_defaults(void)
{
    const char *saved = "{\"tracks\":[],\"led\":{\"count\":0}}";
    boot(saved);
    CHECK_JSON(current(false), "led.count", "24");
    CHECK_EQ_STR(stub_nvs_get(NVS_NS, NVS_KEY), saved); /* kept for a firmware that can read it */
    CHECK_EQ_INT(stub_nvs_writes, 0);
}

/* ---------- changes from the web page ---------- */

static void post_applies_and_saves(void)
{
    boot(NULL);
    uint32_t revision = settings_revision();
    cJSON *doc = current(true);
    set(doc, "tracks.1.volume", "77");
    set(doc, "led.radio.color", "\"rgb\"");
    set(doc, "sensor.playlist.cooldown_s", "99");
    set(doc, "mode", "\"radio\"");
    set(doc, "playlist.sensor_cycles", "5");
    set(doc, "radio.current", "3");
    set(doc, "cast.dlna", "false");
    char err[96];
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_OK);

    cJSON *s = current(false);
    CHECK_JSON(s, "tracks.1.volume", "77");
    CHECK_JSON(s, "led.radio.color", "\"rgb\"");
    CHECK_JSON(s, "sensor.playlist.cooldown_s", "99");
    CHECK_JSON(s, "mode", "\"radio\"");
    CHECK_JSON(s, "playlist.sensor_cycles", "5");
    CHECK_JSON(s, "radio.current", "3");
    CHECK_JSON(s, "cast.dlna", "false");
    CHECK_JSON(s, NULL, stub_nvs_get(NVS_NS, NVS_KEY));
    CHECK_EQ_INT(stub_nvs_writes, 1);
    CHECK_EQ_INT(settings_revision(), revision + 1);
    CHECK_EQ_INT(settings_get_mode(), PLAYER_MODE_RADIO);
}

static void unchanged_settings_are_not_written_again(void)
{
    boot(NULL);
    CHECK_EQ_INT(post_change("led.count", "50"), ESP_OK);
    CHECK_EQ_INT(stub_nvs_writes, 1);
    uint32_t revision = settings_revision();
    CHECK_EQ_INT(post_change("led.count", "50"), ESP_OK);
    CHECK_EQ_INT(stub_nvs_writes, 1);
    CHECK_EQ_INT(settings_revision(), revision);
}

static void defaults_pass_validation(void)
{
    boot(NULL);
    cJSON *doc = current(true);
    char err[96];
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_OK);
}

static void post_requires_current_revision(void)
{
    boot(NULL);
    cJSON *before = current(false);
    char err[96];
    cJSON *doc = current(true);
    char stale[16];
    snprintf(stale, sizeof(stale), "%u", (unsigned)settings_revision() + 1);
    set(doc, "revision", stale);
    set(doc, "led.count", "50");
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_STATE);
    set(doc, "revision", NULL);
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_STATE);
    CHECK(same_json(current(false), before));
    CHECK_EQ_INT(stub_nvs_writes, 0);
}

static void post_rejects_invalid_values(void)
{
    static const struct {
        const char *path;
        const char *json; /* NULL: delete */
        const char *error; /* part of the message */
    } CASES[] = {
        { "tracks", NULL, "tracks" },
        { "led", NULL, "led" },
        { "led.count", "0", "count" },
        { "led.count", "301", "count" },
        { "led.count", "\"24\"", "count" },
        { "led.playlist.color", "\"pink\"", "playlist: color" },
        { "led.radio.min_brightness", "200", "radio: min_brightness is above max_brightness" },
        { "led.playlist.attack_ms", NULL, "playlist: attack_ms" },
        { "led.playlist.release_ms", "5001", "release_ms" },
        { "tracks.0.repeat", "0", "a.mp3: repeat" },
        { "tracks.0.volume", "201", "a.mp3: volume" },
        { "tracks.1.enabled", "1", "b.mp3: enabled" },
        { "tracks.0.name", NULL, "track without name" },
        { "sensor.playlist.trigger", "\"wave\"", "playlist: trigger" },
        { "sensor.radio.max_gate", "9", "radio: max_gate" },
        { "sensor.radio.enabled", NULL, "radio: enabled" },
        { "mode", "\"tv\"", "mode" },
        { "playlist.sensor_cycles", "0", "sensor_cycles" },
        { "playlist.sensor_cycles", "100", "sensor_cycles" },
        { "radio.volume", "-1", "volume" },
        { "radio.sensor_minutes", "0", "sensor_minutes" },
        { "radio.stations", "\"none\"", "stations" },
        { "radio.stations.0.url", "\"ftp://example.com/a.mp3\"", "url must start" },
        { "radio.stations.0.url", NULL, "url" },
        { "radio.stations.0.name", "\"\"", "name" },
        { "radio.current", "11", "current" },
        { "radio.current", "-2", "current" },
        { "cast.airplay", NULL, "airplay" },
        { "cast.spotify", "\"yes\"", "spotify" },
        { "cast.spotify_client_id", "\"xyz\"", "spotify_client_id" },
        { "cast.spotify_client_secret", "\"0123456789abcdef0123456789abcdeg\"", "spotify_client_secret" },
    };

    boot(NULL);
    cJSON *before = current(false);
    char err[96];
    CHECK_EQ_INT(settings_from_json("{\"tracks\":", err, sizeof(err)), ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "invalid JSON") != NULL);

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        cJSON *doc = current(true);
        set(doc, CASES[i].path, CASES[i].json);
        esp_err_t ret = post(doc, err, sizeof(err));
        if (ret != ESP_ERR_INVALID_ARG || strstr(err, CASES[i].error) == NULL) {
            fprintf(stderr, "    %s = %s: returned %d \"%s\", expected an error with \"%s\"\n",
                    CASES[i].path, CASES[i].json ? CASES[i].json : "(deleted)", ret, err, CASES[i].error);
            exit(1);
        }
        cJSON_Delete(doc);
    }

    /* The page shows these as they are */
    cJSON *doc = current(true);
    set(doc, "led.playlist.color", "\"pink\"");
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_ARG);
    CHECK_EQ_STR(err, "playlist: color: expected red, green, blue or rgb");
    set(doc, "led.playlist.color", "\"red\"");
    set(doc, "sensor.radio.trigger", "1");
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_ARG);
    CHECK_EQ_STR(err, "radio: trigger: expected motion or presence");
    set(doc, "sensor.radio.trigger", "\"motion\"");
    set(doc, "tracks.1.pause_ms", "600001");
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_ARG);
    CHECK_EQ_STR(err, "b.mp3: pause_ms: expected a number 0..600000");
    cJSON_Delete(doc);

    /* More stations than fit */
    doc = current(true);
    cJSON *stations = at(doc, "radio.stations");
    while (cJSON_GetArraySize(stations) <= RADIO_MAX_STATIONS) {
        cJSON_AddItemToArray(stations, cJSON_Duplicate(cJSON_GetArrayItem(stations, 0), true));
    }
    CHECK_EQ_INT(post(doc, err, sizeof(err)), ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "stations") != NULL);

    CHECK(same_json(current(false), before));
    CHECK_EQ_INT(stub_nvs_writes, 0);
}

/* The page never gets the Spotify secret; absent in a change, it stays */
static void spotify_secret_stays_on_the_device(void)
{
    boot(FULL);
    cJSON *ui = current(true);
    CHECK(at(ui, "cast.spotify_client_secret") == NULL);
    CHECK_JSON(ui, "cast.spotify_secret_set", "true");
    CHECK_JSON(ui, "cast.spotify_client_id", "\"" CLIENT_ID "\"");

    CHECK_EQ_INT(post_change("cast.spotify", "true"), ESP_OK);
    CHECK_JSON(current(false), "cast.spotify_client_secret", "\"" CLIENT_SECRET "\"");

    CHECK_EQ_INT(post_change("cast.spotify_client_secret", "\"\""), ESP_OK);
    CHECK_JSON(current(false), "cast.spotify_client_secret", "\"\"");
    CHECK_JSON(current(true), "cast.spotify_secret_set", "false");
}

static void ui_json_has_limits_and_extras(void)
{
    boot(NULL);
    cJSON *ui = current(true);
    CHECK_JSON(ui, "revision", "24301"); /* esp_random() of the stub */
    CHECK_JSON(ui, "tracks.0.size", "100005");
    CHECK_JSON(ui, "limits.track", "{\"repeat\":[1,99],\"volume\":[0,200],\"pause_ms\":[0,600000]}");
    CHECK_JSON(ui, "limits.led", "{\"count\":[1,300]}");
    CHECK_JSON(ui, "limits.light", "{\"min_brightness\":[0,255],\"max_brightness\":[0,255],\"idle_brightness\":[0,255],"
                                   "\"attack_ms\":[1,1000],\"release_ms\":[10,5000],\"gain_halflife_ms\":[100,30000]}");
    CHECK_JSON(ui, "limits.sensor", "{\"max_gate\":[2,8],\"sensitivity\":[0,100],\"cooldown_s\":[0,600]}");
    CHECK_JSON(ui, "limits.playlist", "{\"sensor_cycles\":[1,99]}");
    CHECK_JSON(ui, "limits.radio", "{\"volume\":[0,200],\"sensor_minutes\":[1,240]}");
    CHECK_JSON(ui, "limits.max_stations", "16");
    CHECK_JSON(ui, "limits.max_station_name_bytes", "47");
    CHECK_JSON(ui, "limits.max_url_bytes", "255");
    CHECK_JSON(ui, "limits.max_tracks", "32");
    CHECK_JSON(ui, "limits.max_name_bytes", "63");
    CHECK_EQ_INT(cJSON_GetArraySize(at(ui, "limits")), 11);
    CHECK_EQ_INT(cJSON_GetArraySize(at(ui, "station_presets")), 11);
    CHECK_JSON(ui, "storage", "{\"total\":5242880,\"used\":1048576,\"upload_max\":4161536}");
}

/* led_version: re-read the light settings; radar_version: push gate / threshold to the radar */
static void change_counters(void)
{
    boot(NULL);
    uint32_t led = settings_led_version(), radar = settings_radar_version();

    CHECK_EQ_INT(post_change("tracks.0.volume", "10"), ESP_OK);
    CHECK(settings_led_version() == led && settings_radar_version() == radar);

    CHECK_EQ_INT(post_change("led.playlist.max_brightness", "200"), ESP_OK);
    CHECK(settings_led_version() != led && settings_radar_version() == radar);
    led = settings_led_version();

    CHECK_EQ_INT(post_change("sensor.playlist.cooldown_s", "5"), ESP_OK);
    CHECK(settings_led_version() == led && settings_radar_version() == radar);

    CHECK_EQ_INT(post_change("sensor.playlist.max_gate", "4"), ESP_OK);
    CHECK(settings_led_version() == led && settings_radar_version() != radar);
    radar = settings_radar_version();

    CHECK_EQ_INT(post_change("sensor.radio.sensitivity", "70"), ESP_OK); /* not the current mode */
    CHECK(settings_led_version() == led && settings_radar_version() == radar);

    /* Another mode has its own light, and here a different gate */
    CHECK_EQ_INT(post_change("mode", "\"radio\""), ESP_OK);
    CHECK(settings_led_version() != led && settings_radar_version() != radar);
}

static int changes;

static void count_change(void)
{
    changes++;
}

/* Modules that get their configuration pushed hear of accepted changes only */
static void change_listener(void)
{
    boot(NULL);
    settings_on_change(count_change);
    CHECK_EQ_INT(post_change("cast.dlna", "false"), ESP_OK);
    CHECK_EQ_INT(changes, 1);
    CHECK_EQ_INT(post_change("led.count", "0"), ESP_ERR_INVALID_ARG);
    CHECK_EQ_INT(post_change("revision", "1"), ESP_ERR_INVALID_STATE);
    CHECK_EQ_INT(settings_add_track("new.mp3"), ESP_OK);
    CHECK_EQ_INT(changes, 1);
}

/* ---------- next station (web page, BOOT button) ---------- */

static void next_station_goes_round_and_saves(void)
{
    boot(NULL);
    cJSON *s = current(false);
    int n = cJSON_GetArraySize(at(s, "radio.stations"));
    CHECK(n > 2);
    uint32_t revision = settings_revision();

    CHECK(settings_next_station());
    s = current(false);
    CHECK_JSON(s, "radio.current", "1");
    CHECK_JSON(s, NULL, stub_nvs_get(NVS_NS, NVS_KEY));
    CHECK_EQ_INT(settings_revision(), revision + 1);

    for (int i = 2; i < n; i++) {
        CHECK(settings_next_station());
    }
    s = current(false);
    char last[16];
    snprintf(last, sizeof(last), "%d", n - 1);
    CHECK_JSON(s, "radio.current", last);
    CHECK(settings_next_station()); /* after the last: the first */
    s = current(false);
    CHECK_JSON(s, "radio.current", "0");
    radio_station_t st;
    CHECK(settings_get_current_station(&st));
    CHECK_EQ_STR(st.url, cJSON_GetStringValue(at(s, "radio.stations.0.url")));
}

static void next_station_needs_two(void)
{
    boot(NULL);
    CHECK_EQ_INT(post_change("radio.stations", "[{\"name\":\"A\",\"url\":\"http://a\"}]"), ESP_OK);
    int writes = stub_nvs_writes;
    uint32_t revision = settings_revision();
    CHECK(!settings_next_station());
    CHECK_JSON(current(false), "radio.current", "0");
    CHECK_EQ_INT(stub_nvs_writes, writes);
    CHECK_EQ_INT(settings_revision(), revision);
}

/* ---------- track list ---------- */

static void tracks_are_added_and_removed(void)
{
    boot(NULL);
    CHECK_EQ_INT(settings_add_track("0.mp3"), ESP_OK);
    CHECK_EQ_INT(settings_track_count(), 3);
    char name[TRACK_NAME_SIZE];
    track_settings_t ts;
    CHECK(settings_get_track(0, name, &ts));
    CHECK_EQ_STR(name, "0.mp3");
    CHECK(ts.enabled && ts.repeat == 1 && ts.volume == 50 && ts.pause_ms == 1000);
    CHECK_EQ_INT(stub_nvs_writes, 1);
    CHECK_JSON(current(false), NULL, stub_nvs_get(NVS_NS, NVS_KEY));

    CHECK_EQ_INT(settings_add_track("0.mp3"), ESP_OK); /* already listed */
    CHECK_EQ_INT(settings_track_count(), 3);

    settings_remove_track("a.mp3");
    CHECK_EQ_INT(settings_track_count(), 2);
    CHECK(!settings_find_track("a.mp3", &ts));

    for (int i = 0; settings_track_count() < SETTINGS_MAX_TRACKS; i++) {
        snprintf(name, sizeof(name), "t%02d.mp3", i);
        CHECK_EQ_INT(settings_add_track(name), ESP_OK);
    }
    CHECK_EQ_INT(settings_add_track("full.mp3"), ESP_ERR_NO_MEM);
}

static void track_names(void)
{
    CHECK(storage_is_track_name("a.mp3"));
    CHECK(storage_is_track_name("Новый год.MP3"));
    CHECK(storage_is_track_name("12345678901234567890123456789012345678901234567890123456789.mp3")); /* 63 */
    CHECK(!storage_is_track_name("123456789012345678901234567890123456789012345678901234567890.mp3")); /* 64 */
    CHECK(!storage_is_track_name(".a.mp3"));
    CHECK(!storage_is_track_name(".mp3"));
    CHECK(!storage_is_track_name("a.wav"));
    CHECK(!storage_is_track_name("a.mp3.part"));
}

int main(void)
{
    static const test_t tests[] = {
        TEST(defaults_without_saved_settings),
        TEST(saved_settings_load_unchanged),
        TEST(legacy_flat_led_applies_to_both_modes),
        TEST(legacy_flat_sensor_applies_to_both_modes),
        TEST(legacy_cast_without_spotify),
        TEST(light_fields_that_came_later_are_optional),
        TEST(tracks_are_matched_by_name),
        TEST(invalid_saved_settings_fall_back_to_defaults),
        TEST(post_applies_and_saves),
        TEST(unchanged_settings_are_not_written_again),
        TEST(defaults_pass_validation),
        TEST(post_requires_current_revision),
        TEST(post_rejects_invalid_values),
        TEST(spotify_secret_stays_on_the_device),
        TEST(ui_json_has_limits_and_extras),
        TEST(change_counters),
        TEST(change_listener),
        TEST(next_station_goes_round_and_saves),
        TEST(next_station_needs_two),
        TEST(tracks_are_added_and_removed),
        TEST(track_names),
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
