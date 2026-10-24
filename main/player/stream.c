#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "icy.h"
#include "stream.h"
#include "wifi.h"

#define RING_SIZE       (512 * 1024) /* ~30 s of 128 kbit/s, in PSRAM */
#define RX_CHUNK        4096
#define PLAYLIST_MAX    4096         /* .m3u / .pls body read to find the stream URL */
#define MAX_REDIRECTS   5
#define MAX_PLAYLIST_HOPS 3
#define RETRY_MS        3000
#define TIMEOUT_MS      8000
/* Some servers (SomaFM) answer clients without a User-Agent with an announcement instead */
#define USER_AGENT      "NewYearStar/1.0 (ESP32)"

static const char *TAG = "stream";

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

/* Everything about the formats the star plays from a URL. A new one: a stream_codec_t value,
 * a line in CODECS and lines in FORMATS here, its decoder in sdkconfig.defaults, its MIME types
 * in SINK_PROTOCOLS (dlna.c). */
static const struct {
    const char *name;
    esp_audio_simple_dec_type_t decoder;
    bool resyncs; /* the decoder finds frames anywhere, so playing can start mid-file */
} CODECS[] = {
    [STREAM_CODEC_MP3] = { "MP3", ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, true },
    [STREAM_CODEC_AAC] = { "AAC", ESP_AUDIO_SIMPLE_DEC_TYPE_AAC, true },   /* ADTS frames */
    [STREAM_CODEC_FLAC] = { "FLAC", ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC, false },
    [STREAM_CODEC_WAV] = { "WAV", ESP_AUDIO_SIMPLE_DEC_TYPE_WAV, false },
    [STREAM_CODEC_M4A] = { "M4A", ESP_AUDIO_SIMPLE_DEC_TYPE_M4A, false },
};
_Static_assert(ARRAY_SIZE(CODECS) == STREAM_CODEC_COUNT, "a line for every codec");

/* How a format is recognized: a part of the MIME type, else the extension in the URL path */
static const struct {
    const char *mime_part;
    const char *ext;
    stream_codec_t codec;
} FORMATS[] = {
    { "flac", ".flac", STREAM_CODEC_FLAC },
    { "wav", ".wav", STREAM_CODEC_WAV },
    { "audio/mp4", ".m4a", STREAM_CODEC_M4A },
    { "m4a", ".m4a", STREAM_CODEC_M4A },
    { "aac", ".aac", STREAM_CODEC_AAC },
    { "mp4a", ".aacp", STREAM_CODEC_AAC },
    { "mpeg", ".mp3", STREAM_CODEC_MP3 },
    { "mp3", ".mp3", STREAM_CODEC_MP3 },
};

static uint8_t *ring;
static size_t ring_head;
static size_t ring_count;
static uint32_t ring_taken; /* bytes read since the reset, positions the titles */
static SemaphoreHandle_t ring_lock;

/* Song title of a radio station, under ring_lock. It arrives ahead of its audio by what the
 * ring holds (often 10-20 s), so it waits until the player reads up to it. */
static EXT_RAM_BSS_ATTR struct {
    char now[ICY_TITLE_SIZE];
    char next[ICY_TITLE_SIZE];
    uint32_t next_at; /* ring_taken where next starts */
    bool pending;
} title;

static SemaphoreHandle_t req_lock;
static char wanted_url[STREAM_URL_SIZE];
static char wanted_mime[48];
static bool wanted_live;
static uint32_t wanted_offset;
static uint32_t wanted_gen;
static stream_status_t status;
static TaskHandle_t task;
static uint8_t rx_buf[RX_CHUNK];

/* Response headers of the current request, collected by the client's event handler
 * (esp_http_client_get_header() only reads request headers) */
static struct {
    char content_type[64];
    bool accept_ranges;
    uint32_t range_total; /* from Content-Range: bytes a-b/total */
    uint32_t metaint;     /* ICY metadata interval, 0: none */
} resp;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(resp.content_type, evt->header_value, sizeof(resp.content_type));
    } else if (strcasecmp(evt->header_key, "Accept-Ranges") == 0) {
        resp.accept_ranges = strcasestr(evt->header_value, "bytes") != NULL;
    } else if (strcasecmp(evt->header_key, "Content-Range") == 0) {
        const char *slash = strchr(evt->header_value, '/');
        resp.range_total = slash ? strtoul(slash + 1, NULL, 10) : 0;
        resp.accept_ranges = true;
    } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        resp.metaint = strtoul(evt->header_value, NULL, 10);
    }
    return ESP_OK;
}

esp_audio_simple_dec_type_t stream_decoder(stream_codec_t codec)
{
    return CODECS[codec].decoder;
}

bool stream_resyncs(stream_codec_t codec)
{
    return CODECS[codec].resyncs;
}

static size_t ring_write(const uint8_t *data, size_t len)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    size_t n = len < RING_SIZE - ring_count ? len : RING_SIZE - ring_count;
    size_t pos = (ring_head + ring_count) % RING_SIZE;
    size_t first = n < RING_SIZE - pos ? n : RING_SIZE - pos;
    memcpy(ring + pos, data, first);
    memcpy(ring, data + first, n - first);
    ring_count += n;
    xSemaphoreGive(ring_lock);
    return n;
}

size_t stream_read(uint8_t *buf, size_t len)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    size_t n = len < ring_count ? len : ring_count;
    size_t first = n < RING_SIZE - ring_head ? n : RING_SIZE - ring_head;
    memcpy(buf, ring + ring_head, first);
    memcpy(buf + first, ring, n - first);
    ring_head = (ring_head + n) % RING_SIZE;
    ring_count -= n;
    ring_taken += n;
    if (title.pending && (int32_t)(ring_taken - title.next_at) >= 0) {
        strlcpy(title.now, title.next, sizeof(title.now));
        title.pending = false;
    }
    xSemaphoreGive(ring_lock);
    return n;
}

static void ring_reset(void)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    ring_head = 0;
    ring_count = 0;
    ring_taken = 0;
    title.now[0] = '\0';
    title.pending = false;
    xSemaphoreGive(ring_lock);
}

/* From icy_strip(), before the buffer it parses goes into the ring. A title still waiting is
 * replaced: two changes within the buffered seconds show only the later one. */
static void title_received(const char *text, size_t audio_before)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    strlcpy(title.next, text, sizeof(title.next));
    title.next_at = ring_taken + ring_count + audio_before; /* all audio put into the ring so far */
    title.pending = true;
    xSemaphoreGive(ring_lock);
}

static void set_status(uint32_t gen, stream_state_t state, stream_codec_t codec, const char *error)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    if (gen != status.generation) {
        status.content_length = 0;
        status.seekable = false;
    }
    status.generation = gen;
    status.state = state;
    status.codec = codec;
    strlcpy(status.error, error ? error : "", sizeof(status.error));
    xSemaphoreGive(req_lock);
}

static bool changed(uint32_t gen)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    bool ret = wanted_gen != gen;
    xSemaphoreGive(req_lock);
    return ret;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

/* First stream URL in an .m3u (bare URL lines) or .pls (FileN=URL) body */
static bool parse_playlist(char *body, char *url, size_t url_len)
{
    for (char *line = strtok(body, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (isspace((unsigned char)*line)) {
            line++;
        }
        if (strncasecmp(line, "file", 4) == 0 && strchr(line, '=')) {
            line = strchr(line, '=') + 1;
        }
        if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
            strlcpy(url, line, url_len);
            return true;
        }
    }
    return false;
}

static void lower(char *s)
{
    for (; *s; s++) {
        *s = tolower((unsigned char)*s);
    }
}

/* Format from the MIME type, else from the file extension in the URL */
static bool detect_codec(const char *mime, const char *url, stream_codec_t *out)
{
    for (size_t i = 0; mime[0] && i < ARRAY_SIZE(FORMATS); i++) {
        if (strstr(mime, FORMATS[i].mime_part)) {
            *out = FORMATS[i].codec;
            return true;
        }
    }
    char path[STREAM_URL_SIZE];
    strlcpy(path, url, sizeof(path));
    path[strcspn(path, "?#")] = '\0';
    for (size_t i = 0; i < ARRAY_SIZE(FORMATS); i++) {
        if (ends_with(path, FORMATS[i].ext)) {
            *out = FORMATS[i].codec;
            return true;
        }
    }
    return false;
}

/* Connect, follow redirects and playlists, then pump the stream into the ring until the
 * request changes (returns NULL), the file ends (returns NULL, *ended) or something fails
 * (returns the reason). A live stream is asked for its song titles. */
static const char *receive(const char *start_url, const char *mime_hint, bool live, uint32_t offset,
                           uint32_t gen, bool *ended)
{
    static char url[STREAM_URL_SIZE];
    static char error[96];
    static EXT_RAM_BSS_ATTR icy_t icy;
    strlcpy(url, start_url, sizeof(url));

    for (int hop = 0; hop < MAX_PLAYLIST_HOPS; hop++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .user_agent = USER_AGENT,
            .timeout_ms = TIMEOUT_MS,
            .buffer_size = 2048, /* response headers of streaming servers can be long */
            .crt_bundle_attach = esp_crt_bundle_attach,
            .max_redirection_count = MAX_REDIRECTS,
            .event_handler = http_event,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            return "Нет памяти для соединения";
        }
        if (offset) {
            char range[32];
            snprintf(range, sizeof(range), "bytes=%" PRIu32 "-", offset);
            esp_http_client_set_header(client, "Range", range);
        }
        if (live) {
            esp_http_client_set_header(client, "Icy-MetaData", "1");
        }

        int code = 0;
        const char *fail = NULL;
        for (int redirects = 0;; redirects++) {
            memset(&resp, 0, sizeof(resp));
            if (esp_http_client_open(client, 0) != ESP_OK) {
                fail = "Сервер недоступен";
                break;
            }
            if (esp_http_client_fetch_headers(client) < 0) {
                fail = "Сервер не ответил";
                break;
            }
            code = esp_http_client_get_status_code(client);
            if (code < 300 || code >= 400 || redirects == MAX_REDIRECTS) {
                break;
            }
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
        }
        if (fail == NULL && code != 200 && !(code == 206 && offset)) {
            snprintf(error, sizeof(error), "Сервер ответил кодом %d", code);
            fail = error;
        }
        if (fail) {
            esp_http_client_cleanup(client);
            return fail;
        }

        char content_type[64];
        strlcpy(content_type, resp.content_type, sizeof(content_type));
        lower(content_type);
        if (strstr(content_type, "octet-stream")) {
            content_type[0] = '\0'; /* says nothing about the format */
        }

        if (strstr(content_type, "mpegurl") || strstr(content_type, "scpls") ||
            ends_with(url, ".m3u") || ends_with(url, ".pls")) {
            static char body[PLAYLIST_MAX + 1];
            int len = esp_http_client_read(client, body, PLAYLIST_MAX);
            esp_http_client_cleanup(client);
            body[len > 0 ? len : 0] = '\0';
            if (strstr(body, "#EXT-X-")) {
                return "HLS-потоки не поддерживаются";
            }
            if (!parse_playlist(body, url, sizeof(url))) {
                return "В плейлисте нет ссылки на поток";
            }
            ESP_LOGI(TAG, "Playlist points to %s", url);
            continue;
        }

        stream_codec_t codec = STREAM_CODEC_MP3; /* unknown format: most streams are MP3 */
        if (!detect_codec(content_type, url, &codec) && !detect_codec(mime_hint, url, &codec) &&
            content_type[0] && !strstr(content_type, "audio")) {
            esp_http_client_cleanup(client);
            snprintf(error, sizeof(error), "Формат не поддерживается: %.40s", content_type);
            return error;
        }

        if (offset && code == 200) {
            ESP_LOGW(TAG, "Server ignored the range, playing from the start");
        }
        int64_t length = esp_http_client_get_content_length(client);
        uint32_t total = code == 206 ? resp.range_total : length > 0 ? (uint32_t)length : 0;

        ESP_LOGI(TAG, "Streaming %s (%s, %" PRIu32 " bytes%s%s)", url, CODECS[codec].name, total,
                 resp.accept_ranges ? ", seekable" : "", code == 206 ? ", from offset" : "");
        set_status(gen, STREAM_STREAMING, codec, NULL);
        xSemaphoreTake(req_lock, portMAX_DELAY);
        status.content_length = total;
        status.seekable = resp.accept_ranges && total > 0;
        xSemaphoreGive(req_lock);
        icy_init(&icy, resp.metaint);
        fail = NULL;
        while (!changed(gen)) {
            int n = esp_http_client_read(client, (char *)rx_buf, sizeof(rx_buf));
            if (n == 0 && esp_http_client_is_complete_data_received(client)) {
                *ended = true;
                break;
            }
            if (n <= 0) {
                fail = "Поток прервался";
                break;
            }
            n = (int)icy_strip(&icy, rx_buf, n, title_received);
            /* Ring full: wait for the player, the stream is real-time anyway */
            for (int off = 0; off < n && !changed(gen);) {
                off += ring_write(rx_buf + off, n - off);
                if (off < n) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        esp_http_client_cleanup(client);
        return fail;
    }
    return "Слишком много вложенных плейлистов";
}

/* A notification can be left over from an earlier stream_play(): only a new generation counts,
 * otherwise a finished file would be downloaded again right away */
static void wait_for_new_request(uint32_t gen)
{
    while (!changed(gen)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void stream_task(void *arg)
{
    for (;;) {
        static char url[STREAM_URL_SIZE];
        char mime[sizeof(wanted_mime)];
        xSemaphoreTake(req_lock, portMAX_DELAY);
        strlcpy(url, wanted_url, sizeof(url));
        strlcpy(mime, wanted_mime, sizeof(mime));
        bool live = wanted_live;
        uint32_t offset = wanted_offset;
        uint32_t gen = wanted_gen;
        xSemaphoreGive(req_lock);
        lower(mime);

        ring_reset(); /* before the status names the new generation */
        if (!url[0]) {
            set_status(gen, STREAM_STOPPED, STREAM_CODEC_MP3, NULL);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* A live stream keeps reconnecting, the buffered data stays valid; a file is
         * received once (a cast file can also come from a phone on the star's own network) */
        while (!changed(gen)) {
            if (live && !wifi_sta_connected()) {
                set_status(gen, STREAM_ERROR, STREAM_CODEC_MP3, "Нет подключения к Wi-Fi");
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
            set_status(gen, STREAM_CONNECTING, STREAM_CODEC_MP3, NULL);
            bool ended = false;
            const char *error = receive(url, mime, live, offset, gen, &ended);
            if (changed(gen)) {
                break;
            }
            if (ended) {
                ESP_LOGI(TAG, "%s received completely", url);
                stream_status_t st;
                stream_get_status(&st);
                set_status(gen, STREAM_ENDED, st.codec, NULL);
                wait_for_new_request(gen);
                break;
            }
            ESP_LOGW(TAG, "%s: %s", url, error ? error : "stopped");
            stream_status_t st;
            stream_get_status(&st);
            set_status(gen, STREAM_ERROR, st.codec, error);
            if (!live) {
                wait_for_new_request(gen);
                break;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        }
    }
}

void stream_start(void)
{
    ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
    ring_lock = xSemaphoreCreateMutex();
    req_lock = xSemaphoreCreateMutex();
    assert(ring && ring_lock && req_lock);
    status.buffer_size = RING_SIZE;
    /* TLS handshakes need a deep stack */
    xTaskCreateWithCaps(stream_task, "stream", 8192, NULL, 6, &task, MALLOC_CAP_SPIRAM);
}

uint32_t stream_play(const char *url, const char *mime_hint, bool live, uint32_t offset)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    strlcpy(wanted_url, url, sizeof(wanted_url));
    strlcpy(wanted_mime, mime_hint ? mime_hint : "", sizeof(wanted_mime));
    wanted_live = live;
    wanted_offset = offset;
    uint32_t gen = ++wanted_gen;
    xSemaphoreGive(req_lock);
    xTaskNotifyGive(task);
    return gen;
}

void stream_stop(void)
{
    stream_play("", NULL, false, 0);
}

uint32_t stream_generation(void)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    uint32_t gen = wanted_gen;
    xSemaphoreGive(req_lock);
    return gen;
}

void stream_get_status(stream_status_t *out)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(req_lock);
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    out->buffered = ring_count;
    xSemaphoreGive(ring_lock);
}

static const char *state_name(stream_state_t s)
{
    switch (s) {
    case STREAM_STOPPED: return "stopped";
    case STREAM_CONNECTING: return "connecting";
    case STREAM_STREAMING: return "streaming";
    case STREAM_ERROR: return "error";
    case STREAM_ENDED: return "ended";
    }
    return "unknown";
}

void stream_status_json(cJSON *out)
{
    stream_status_t st;
    stream_get_status(&st);
    cJSON_AddStringToObject(out, "state", state_name(st.state));
    cJSON_AddStringToObject(out, "codec", CODECS[st.codec].name);
    cJSON_AddNumberToObject(out, "buffered", st.buffered);
    cJSON_AddNumberToObject(out, "buffer_size", st.buffer_size);
    cJSON_AddStringToObject(out, "error", st.error);
    char song[ICY_TITLE_SIZE];
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    strlcpy(song, title.now, sizeof(song));
    xSemaphoreGive(ring_lock);
    cJSON_AddStringToObject(out, "title", song);
}
