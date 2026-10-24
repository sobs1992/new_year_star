#include "nvs_flash.h"
#include "esp_err.h"
#include "button.h"
#include "led_music.h"
#include "player.h"
#include "sensor.h"
#include "settings.h"
#include "status.h"
#include "storage.h"
#include "web.h"
#include "wifi.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    storage_init(); /* without it there are no tracks, but LEDs and web UI still work */
    settings_init();
    led_music_start();
    sensor_start();
    wifi_start();
    button_start();
    player_start();
    web_start();
    status_start();
}
