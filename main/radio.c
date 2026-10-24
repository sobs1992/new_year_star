#include <ctype.h>
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
#include "radio.h"
#include "settings.h"
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

static const char *TAG = "radio";

static uint8_t *ring;
static size_t ring_head;
static size_t ring_count;
static SemaphoreHandle_t ring_lock;

static SemaphoreHandle_t req_lock;
static char wanted_url[RADIO_URL_SIZE];
static uint32_t wanted_gen;
static radio_status_t status;
static TaskHandle_t task;
static uint8_t rx_buf[RX_CHUNK];

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

size_t radio_read(uint8_t *buf, size_t len)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    size_t n = len < ring_count ? len : ring_count;
    size_t first = n < RING_SIZE - ring_head ? n : RING_SIZE - ring_head;
    memcpy(buf, ring + ring_head, first);
    memcpy(buf + first, ring, n - first);
    ring_head = (ring_head + n) % RING_SIZE;
    ring_count -= n;
    xSemaphoreGive(ring_lock);
    return n;
}

static void ring_reset(void)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    ring_head = 0;
    ring_count = 0;
    xSemaphoreGive(ring_lock);
}

static void set_status(uint32_t gen, radio_state_t state, radio_codec_t codec, const char *error)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
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

/* Connect, follow redirects and playlists, then pump the stream into the ring until the
 * request changes (returns NULL) or something fails (returns the reason) */
static const char *stream(const char *start_url, uint32_t gen)
{
    static char url[RADIO_URL_SIZE];
    static char error[96];
    strlcpy(url, start_url, sizeof(url));

    for (int hop = 0; hop < MAX_PLAYLIST_HOPS; hop++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .user_agent = USER_AGENT,
            .timeout_ms = TIMEOUT_MS,
            .buffer_size = 2048, /* response headers of streaming servers can be long */
            .crt_bundle_attach = esp_crt_bundle_attach,
            .max_redirection_count = MAX_REDIRECTS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            return "Нет памяти для соединения";
        }

        int code = 0;
        const char *fail = NULL;
        for (int redirects = 0;; redirects++) {
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
        if (fail == NULL && code != 200) {
            snprintf(error, sizeof(error), "Сервер ответил кодом %d", code);
            fail = error;
        }
        if (fail) {
            esp_http_client_cleanup(client);
            return fail;
        }

        char *type = NULL;
        esp_http_client_get_header(client, "Content-Type", &type);
        char content_type[64];
        strlcpy(content_type, type ? type : "", sizeof(content_type));
        for (char *c = content_type; *c; c++) {
            *c = tolower((unsigned char)*c);
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

        radio_codec_t codec;
        if (strstr(content_type, "aac") || strstr(content_type, "mp4a") ||
            (!content_type[0] && (ends_with(url, ".aac") || ends_with(url, ".aacp")))) {
            codec = RADIO_CODEC_AAC;
        } else if (strstr(content_type, "mpeg") || strstr(content_type, "mp3") || !content_type[0]) {
            codec = RADIO_CODEC_MP3;
        } else {
            esp_http_client_cleanup(client);
            snprintf(error, sizeof(error), "Формат не поддерживается: %.40s", content_type);
            return error;
        }

        ESP_LOGI(TAG, "Streaming %s (%s)", url, codec == RADIO_CODEC_AAC ? "AAC" : "MP3");
        set_status(gen, RADIO_STREAMING, codec, NULL);
        fail = NULL;
        while (!changed(gen)) {
            int n = esp_http_client_read(client, (char *)rx_buf, sizeof(rx_buf));
            if (n <= 0) {
                fail = "Поток прервался";
                break;
            }
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

static void radio_task(void *arg)
{
    for (;;) {
        char url[RADIO_URL_SIZE];
        xSemaphoreTake(req_lock, portMAX_DELAY);
        strlcpy(url, wanted_url, sizeof(url));
        uint32_t gen = wanted_gen;
        xSemaphoreGive(req_lock);

        ring_reset(); /* before the status names the new generation */
        if (!url[0]) {
            set_status(gen, RADIO_STOPPED, RADIO_CODEC_MP3, NULL);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* Same request: keep reconnecting, the buffered data stays valid */
        while (!changed(gen)) {
            if (!wifi_sta_connected()) {
                set_status(gen, RADIO_ERROR, RADIO_CODEC_MP3, "Нет подключения к Wi-Fi");
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
            set_status(gen, RADIO_CONNECTING, RADIO_CODEC_MP3, NULL);
            const char *error = stream(url, gen);
            if (error == NULL || changed(gen)) {
                break;
            }
            ESP_LOGW(TAG, "%s: %s", url, error);
            set_status(gen, RADIO_ERROR, RADIO_CODEC_MP3, error);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        }
    }
}

void radio_init(void)
{
    ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
    ring_lock = xSemaphoreCreateMutex();
    req_lock = xSemaphoreCreateMutex();
    assert(ring && ring_lock && req_lock);
    status.buffer_size = RING_SIZE;
    /* TLS handshakes need a deep stack */
    xTaskCreate(radio_task, "radio", 8192, NULL, 6, &task);
}

uint32_t radio_play(const char *url)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    strlcpy(wanted_url, url, sizeof(wanted_url));
    uint32_t gen = ++wanted_gen;
    xSemaphoreGive(req_lock);
    xTaskNotifyGive(task);
    return gen;
}

void radio_stop(void)
{
    radio_play("");
}

uint32_t radio_generation(void)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    uint32_t gen = wanted_gen;
    xSemaphoreGive(req_lock);
    return gen;
}

void radio_get_status(radio_status_t *out)
{
    xSemaphoreTake(req_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(req_lock);
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    out->buffered = ring_count;
    xSemaphoreGive(ring_lock);
}
