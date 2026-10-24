#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Own access point (always on, captive portal) plus an optional home network (station) */

#define WIFI_HOSTNAME "star" /* http://star.local in the home network */
#define WIFI_AP_SSID  "NewYearStar"

typedef enum {
    WIFI_STA_OFF,        /* no home network configured */
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_FAILED,     /* retrying, see reason */
} wifi_sta_state_t;

typedef enum {
    WIFI_FAIL_NONE,
    WIFI_FAIL_NOT_FOUND,
    WIFI_FAIL_WRONG_PASSWORD,
    WIFI_FAIL_OTHER,
} wifi_fail_reason_t;

typedef struct {
    wifi_sta_state_t state;
    wifi_fail_reason_t reason;
    char ssid[33];
    char ip[16];
    int8_t rssi;
} wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_network_t;

/* Start the access point and connect to the saved home network, if any */
void wifi_start(void);

/* Save and connect to a home network; empty ssid forgets it */
esp_err_t wifi_set_network(const char *ssid, const char *password);

/* Password of the star's own network: empty makes it open, otherwise 8..63 characters (WPA2).
 * Saved, and applied a second later: clients of the access point get disconnected. */
esp_err_t wifi_set_ap_password(const char *password);
bool wifi_ap_secure(void);

void wifi_get_status(wifi_status_t *out);
bool wifi_sta_connected(void);

/* Blocking scan (~2 s); returns the number of networks written, or -1 on failure */
int wifi_scan(wifi_network_t *out, int max);
