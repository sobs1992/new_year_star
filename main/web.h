#pragma once

#include "esp_http_server.h"

/* HTTP server with the settings page and API, plus the captive portal DNS (wifi_start first).
 * DLNA and Spotify add their handlers to the server it returns. */
httpd_handle_t web_start(void);
