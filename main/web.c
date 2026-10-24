#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h" /* dns_server.h needs its types */
#include "cJSON.h"
#include "airplay.h"
#include "spotify.h"
#include "dns_server.h"
#include "player.h"
#include "stream.h"
#include "sensor.h"
#include "settings.h"
#include "status.h"
#include "sysinfo.h"
#include "tracks.h"
#include "web.h"
#include "wifi.h"

#define MAX_BODY_SIZE   16384
#define SCAN_MAX        20
#define UPLOAD_CHUNK    4096

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char system_html_start[] asm("_binary_system_html_start");
extern const char system_html_end[] asm("_binary_system_html_end");

static const char *TAG = "web";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t system_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, system_html_start, system_html_end - system_html_start);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* Send a document and delete it */
static esp_err_t send_cjson(httpd_req_t *req, const char *status, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t ret = send_json(req, status, json);
    free(json);
    return ret;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "error", message);
    return send_cjson(req, status, resp);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    char *json = settings_to_json(true);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t ret = send_json(req, "200 OK", json);
    free(json);
    return ret;
}

/* Whole small request body as a string; caller frees. NULL on bad size or broken connection. */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_SIZE) {
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    size_t got = 0;
    while (body && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(body);
            return NULL;
        }
        got += r;
    }
    if (body) {
        body[got] = '\0';
    }
    return body;
}

/* The request body parsed, NULL when missing or not JSON; caller deletes */
static cJSON *read_json_body(httpd_req_t *req)
{
    char *body = read_body(req);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);
    return root;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "Bad request body");
    }

    char err[96];
    esp_err_t ret = settings_from_json(body, err, sizeof(err));
    free(body);
    if (ret == ESP_OK) {
        char resp[48];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"revision\":%" PRIu32 "}", settings_revision());
        return send_json(req, "200 OK", resp);
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Настройки изменились в другом месте");
    }

    ESP_LOGW(TAG, "Settings rejected: %s", err);
    return send_error(req, "400 Bad Request", err);
}

/* Live state for the page, each part from its module */
static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    player_status_json(cJSON_AddObjectToObject(root, "player"));
    sensor_status_json(cJSON_AddObjectToObject(root, "sensor"));
    wifi_status_json(cJSON_AddObjectToObject(root, "wifi"));
    status_errors_json(cJSON_AddArrayToObject(root, "errors"));
    stream_status_json(cJSON_AddObjectToObject(root, "radio")); /* also while DLNA plays */
    airplay_status_json(cJSON_AddObjectToObject(root, "airplay"));
    spotify_status_json(cJSON_AddObjectToObject(root, "spotify"));
    sysinfo_heap_json(cJSON_AddObjectToObject(root, "heap"));
    return send_cjson(req, "200 OK", root);
}

static int hex_value(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

/* "name" query parameter, URL-decoded; false if missing, too long or a path */
static bool get_track_name(httpd_req_t *req, char name[TRACK_NAME_SIZE])
{
    char query[3 * TRACK_NAME_SIZE + 16];
    char encoded[3 * TRACK_NAME_SIZE];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded, sizeof(encoded)) != ESP_OK) {
        return false;
    }
    size_t n = 0;
    for (const char *p = encoded; *p; p++) {
        char c = *p;
        if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            c = hex_value(p[1]) << 4 | hex_value(p[2]);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (n + 1 >= TRACK_NAME_SIZE || c == '/' || c == '\\' || c == '\0') {
            return false;
        }
        name[n++] = c;
    }
    name[n] = '\0';
    return true;
}

/* POST /api/tracks?name=<file.mp3> with the file as the raw body */
static esp_err_t track_upload(httpd_req_t *req)
{
    char name[TRACK_NAME_SIZE];
    char msg[96];
    if (!get_track_name(req, name)) {
        name[0] = '\0'; /* tracks_upload_begin() explains what the name must be */
    }
    uint8_t *buf = malloc(UPLOAD_CHUNK);
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (buf == NULL) {
        strlcpy(msg, "Не удалось начать запись", sizeof(msg));
    } else {
        ret = tracks_upload_begin(name, req->content_len, msg, sizeof(msg));
    }

    size_t received = 0;
    while (ret == ESP_OK && received < req->content_len) {
        size_t want = req->content_len - received;
        int r = httpd_req_recv(req, (char *)buf, want < UPLOAD_CHUNK ? want : UPLOAD_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            tracks_upload_abort();
            strlcpy(msg, "Загрузка прервалась", sizeof(msg));
            ret = ESP_FAIL;
            break;
        }
        ret = tracks_upload_write(buf, r, msg, sizeof(msg));
        received += r;
    }
    free(buf);
    if (ret == ESP_OK) {
        ret = tracks_upload_finish(msg, sizeof(msg));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Upload of %s failed: %s", name, msg);
        return send_error(req, ret == ESP_ERR_INVALID_STATE ? "409 Conflict" :
                               ret == ESP_ERR_INVALID_SIZE ? "413 Content Too Large" : "400 Bad Request", msg);
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* DELETE /api/tracks?name=<file.mp3> */
static esp_err_t track_delete(httpd_req_t *req)
{
    char name[TRACK_NAME_SIZE];
    esp_err_t ret = get_track_name(req, name) ? tracks_delete(name) : ESP_ERR_NOT_FOUND;
    if (ret == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "Нет такого файла");
    }
    if (ret != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Не удалось удалить файл");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/player {"action": "start" | "stop"} */
static esp_err_t player_post(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    bool ok = cJSON_IsString(action);
    if (ok && strcmp(action->valuestring, "start") == 0) {
        player_play();
    } else if (ok && strcmp(action->valuestring, "stop") == 0) {
        player_stop();
    } else {
        ok = false;
    }
    cJSON_Delete(root);
    if (!ok) {
        return send_error(req, "400 Bad Request", "action: expected \"start\" or \"stop\"");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/wifi {"ssid": "...", "password": "..."}; empty ssid forgets the network */
static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
        ret = wifi_set_network(ssid->valuestring, password->valuestring);
    }
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        return send_error(req, "400 Bad Request", "Имя сети до 32 байт, пароль до 63 символов");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/wifi/ap {"password": "..."}: password of the star's own network, empty: open */
static esp_err_t wifi_ap_post(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    esp_err_t ret = cJSON_IsString(password) ? wifi_set_ap_password(password->valuestring)
                                             : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "Пароль: пустой или от 8 до 63 символов");
    }
    if (ret != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Не удалось сохранить пароль");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* GET /api/wifi/scan: networks around, strongest first */
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_network_t *nets = calloc(SCAN_MAX, sizeof(*nets));
    int n = nets ? wifi_scan(nets, SCAN_MAX) : -1;
    if (n < 0) {
        free(nets);
        return send_error(req, "503 Service Unavailable", "Не удалось найти сети, попробуйте ещё раз");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *jn = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(j, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(j, "secure", nets[i].secure);
        cJSON_AddItemToArray(jn, j);
    }
    free(nets);
    return send_cjson(req, "200 OK", root);
}

/* System page data: CPU load, memory, temperature */
static esp_err_t system_get(httpd_req_t *req)
{
    char *json = sysinfo_json();
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t ret = send_json(req, "200 OK", json);
    free(json);
    return ret;
}

/* Any unknown URL (phone connectivity checks included) goes to the settings page */
static esp_err_t redirect_to_portal(httpd_req_t *req, httpd_err_code_t err)
{
    if (req->method != HTTP_GET) {
        ESP_LOGW(TAG, "No handler for %s %s", http_method_str(req->method), req->uri);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/"); /* relative: works on the star network and at home */
    /* iOS needs a body to detect the captive portal */
    return httpd_resp_sendstr(req, "Redirect to the star settings");
}

static httpd_handle_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 16;
    cfg.max_uri_handlers = 24; /* 12 here, DLNA 4, Spotify 2 */
    cfg.uri_match_fn = httpd_uri_match_wildcard; /* /dlna/... handlers */
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/tracks", .method = HTTP_POST, .handler = track_upload },
        { .uri = "/api/tracks", .method = HTTP_DELETE, .handler = track_delete },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/wifi/ap", .method = HTTP_POST, .handler = wifi_ap_post },
        { .uri = "/api/player", .method = HTTP_POST, .handler = player_post },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get },
        { .uri = "/system", .method = HTTP_GET, .handler = system_page_get },
        { .uri = "/api/system", .method = HTTP_GET, .handler = system_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_portal));
    return server;
}

httpd_handle_t web_start(void)
{
    /* Redirected foreign traffic makes these noisy */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_handle_t server = http_start();

    /* Answer every DNS query with our IP, so any site opens the portal */
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&dns_cfg);
    return server;
}
