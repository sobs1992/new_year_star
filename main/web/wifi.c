#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "nvs.h"
#include "cJSON.h"
#include "app_config.h"
#include "wifi.h"

#define AP_MAX_CONN     4
#define AP_APPLY_MS     1000 /* let the HTTP answer leave before clients get dropped */
#define NVS_AP_PASSWORD "ap_password"
#define RECONNECT_MS    5000
#define SCAN_MAX        24

static const char *TAG = "wifi";

static char portal_url[32]; /* DHCP option 114 keeps this pointer */
static esp_netif_t *sta_netif;
static esp_timer_handle_t reconnect_timer;
static esp_timer_handle_t ap_apply_timer;
static char ap_password[64]; /* empty: open network */
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t status;
static atomic_bool scanning; /* don't reconnect in the middle of a scan */

static void set_state(wifi_sta_state_t state, wifi_fail_reason_t reason, const char *ip)
{
    portENTER_CRITICAL(&status_lock);
    status.state = state;
    status.reason = reason;
    strlcpy(status.ip, ip ? ip : "", sizeof(status.ip));
    portEXIT_CRITICAL(&status_lock);
}

static wifi_fail_reason_t fail_reason(uint8_t code)
{
    switch (code) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_FAIL_NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return WIFI_FAIL_WRONG_PASSWORD;
    default:
        return WIFI_FAIL_OTHER;
    }
}

static void reconnect(void *arg)
{
    if (!scanning && status.ssid[0]) {
        esp_wifi_connect();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        char ip[16];
        inet_ntoa_r(e->ip_info.ip.addr, ip, sizeof(ip));
        set_state(WIFI_STA_CONNECTED, WIFI_FAIL_NONE, ip);
        ESP_LOGI(TAG, "Connected to \"%s\", IP %s, http://%s.local/", status.ssid, ip, WIFI_HOSTNAME);
        return;
    }
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "Client " MACSTR " joined the star network", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        if (!status.ssid[0] || scanning) {
            break; /* forgotten network, or disconnected on purpose for a scan */
        }
        ESP_LOGW(TAG, "Home network \"%s\" lost, reason %d", status.ssid, e->reason);
        set_state(WIFI_STA_FAILED, fail_reason(e->reason), NULL);
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, RECONNECT_MS * 1000);
        break;
    }
    default:
        break;
    }
}

static void ap_apply(void *arg)
{
    wifi_config_t cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .max_connection = AP_MAX_CONN,
            .authmode = ap_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)cfg.ap.password, ap_password, sizeof(cfg.ap.password));
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Access point config failed: %s", esp_err_to_name(ret));
    }
}

static void load_ap_password(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(ap_password);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_AP_PASSWORD, ap_password, &len) != ESP_OK) {
            ap_password[0] = '\0';
        }
        nvs_close(nvs);
    }
}

static void start_ap(esp_netif_t *netif)
{
    load_ap_password();
    ap_apply(NULL);

    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip));
    char ip_str[16];
    inet_ntoa_r(ip.ip.addr, ip_str, sizeof(ip_str));
    snprintf(portal_url, sizeof(portal_url), "http://%s/", ip_str);

    /* DHCP option 114 tells modern phones the portal URL directly */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           portal_url, strlen(portal_url)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));

    ESP_LOGI(TAG, "Access point \"%s\" (%s), settings at %s", WIFI_AP_SSID,
             ap_password[0] ? "WPA2" : "open", portal_url);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS failed, %s.local will not resolve", WIFI_HOSTNAME);
        return;
    }
    mdns_hostname_set(WIFI_HOSTNAME);
    mdns_instance_name_set("New Year Star");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, WIFI_HOSTNAME);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    const esp_timer_create_args_t timer_args = { .callback = reconnect, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));
    const esp_timer_create_args_t ap_timer_args = { .callback = ap_apply, .name = "wifi_ap_apply" };
    ESP_ERROR_CHECK(esp_timer_create(&ap_timer_args, &ap_apply_timer));

    /* AP+STA always: the station side is needed for scanning even without a home network */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    start_ap(ap_netif);

    /* The driver keeps the station config in NVS */
    wifi_config_t sta;
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK && sta.sta.ssid[0]) {
        strlcpy(status.ssid, (const char *)sta.sta.ssid, sizeof(status.ssid));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Mains powered: no modem sleep. With it the radio wakes only at the router's beacons,
     * every request waits up to ~300 ms, and DLNA apps that send a series of requests per
     * tap feel sluggish. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    start_mdns();

    if (status.ssid[0]) {
        ESP_LOGI(TAG, "Connecting to \"%s\"", status.ssid);
        set_state(WIFI_STA_CONNECTING, WIFI_FAIL_NONE, NULL);
        esp_wifi_connect();
    }
}

esp_err_t wifi_set_network(const char *ssid, const char *password)
{
    if (strlen(ssid) > 32 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_timer_stop(reconnect_timer);
    portENTER_CRITICAL(&status_lock);
    status.ssid[0] = '\0'; /* the disconnect below must not trigger a reconnect */
    portEXIT_CRITICAL(&status_lock);
    esp_wifi_disconnect();

    wifi_config_t sta = { 0 };
    memcpy(sta.sta.ssid, ssid, strlen(ssid));
    memcpy(sta.sta.password, password, strlen(password));
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&status_lock);
    strlcpy(status.ssid, ssid, sizeof(status.ssid));
    portEXIT_CRITICAL(&status_lock);
    if (!ssid[0]) {
        ESP_LOGI(TAG, "Home network forgotten");
        set_state(WIFI_STA_OFF, WIFI_FAIL_NONE, NULL);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);
    set_state(WIFI_STA_CONNECTING, WIFI_FAIL_NONE, NULL);
    return esp_wifi_connect();
}

esp_err_t wifi_set_ap_password(const char *password)
{
    size_t len = strlen(password);
    if (len != 0 && (len < 8 || len > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, NVS_AP_PASSWORD, password);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    strlcpy(ap_password, password, sizeof(ap_password));
    ESP_LOGI(TAG, "Access point is now %s", len ? "protected by a password" : "open");
    /* Reconfiguring the AP drops its clients, maybe the one asking: answer it first */
    esp_timer_stop(ap_apply_timer);
    return esp_timer_start_once(ap_apply_timer, AP_APPLY_MS * 1000);
}

bool wifi_ap_secure(void)
{
    return ap_password[0] != '\0';
}

void wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&status_lock);
    *out = status;
    portEXIT_CRITICAL(&status_lock);
    wifi_ap_record_t ap;
    out->rssi = out->state == WIFI_STA_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

bool wifi_sta_connected(void)
{
    return status.state == WIFI_STA_CONNECTED;
}

static int compare_rssi(const void *a, const void *b)
{
    return ((const wifi_network_t *)b)->rssi - ((const wifi_network_t *)a)->rssi;
}

int wifi_scan(wifi_network_t *out, int max)
{
    /* A scan fails while the station is busy connecting, so pause that */
    scanning = true;
    if (status.state != WIFI_STA_CONNECTED) {
        esp_timer_stop(reconnect_timer);
        esp_wifi_disconnect();
    }
    esp_err_t ret = esp_wifi_scan_start(NULL, true);

    int n = -1;
    uint16_t found = SCAN_MAX;
    wifi_ap_record_t *records = calloc(SCAN_MAX, sizeof(*records));
    if (ret == ESP_OK && records && esp_wifi_scan_get_ap_records(&found, records) == ESP_OK) {
        n = 0;
        for (int i = 0; i < found && n < max; i++) {
            const char *ssid = (const char *)records[i].ssid;
            bool duplicate = false;
            for (int j = 0; j < n; j++) {
                duplicate |= strcmp(out[j].ssid, ssid) == 0;
            }
            if (!ssid[0] || duplicate) {
                continue; /* hidden network, or one more access point of a mesh */
            }
            strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
            out[n].rssi = records[i].rssi;
            out[n].secure = records[i].authmode != WIFI_AUTH_OPEN;
            n++;
        }
        qsort(out, n, sizeof(*out), compare_rssi);
    } else {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(ret));
        esp_wifi_clear_ap_list();
    }
    free(records);

    scanning = false;
    if (status.ssid[0] && status.state != WIFI_STA_CONNECTED) {
        esp_wifi_connect();
    }
    return n;
}

static const char *state_name(wifi_sta_state_t s)
{
    switch (s) {
    case WIFI_STA_OFF: return "off";
    case WIFI_STA_CONNECTING: return "connecting";
    case WIFI_STA_CONNECTED: return "connected";
    case WIFI_STA_FAILED: return "failed";
    }
    return "unknown";
}

static const char *reason_name(wifi_fail_reason_t r)
{
    switch (r) {
    case WIFI_FAIL_NONE: return "";
    case WIFI_FAIL_NOT_FOUND: return "not_found";
    case WIFI_FAIL_WRONG_PASSWORD: return "wrong_password";
    case WIFI_FAIL_OTHER: return "other";
    }
    return "other";
}

void wifi_status_json(cJSON *out)
{
    wifi_status_t ws;
    wifi_get_status(&ws);
    cJSON_AddStringToObject(out, "state", state_name(ws.state));
    cJSON_AddStringToObject(out, "reason", reason_name(ws.reason));
    cJSON_AddStringToObject(out, "ssid", ws.ssid);
    cJSON_AddStringToObject(out, "ip", ws.ip);
    cJSON_AddNumberToObject(out, "rssi", ws.rssi);
    cJSON_AddStringToObject(out, "hostname", WIFI_HOSTNAME ".local");
    cJSON_AddStringToObject(out, "ap_ssid", WIFI_AP_SSID);
    cJSON_AddBoolToObject(out, "ap_secure", wifi_ap_secure());
}
