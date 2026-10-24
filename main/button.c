#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "app_config.h"
#include "board.h"
#include "button.h"
#include "led_music.h"
#include "status.h"

#define POLL_MS         50
#define FEEDBACK_MS     1000        /* start blinking after this, so a short press shows nothing */
#define RESET_MS        5000
#define BLINK_MS        250
#define SIGNAL_COLOR    0x606060    /* dim white: the whole strip at full white draws over 1 A */

static const char *TAG = "button";

static void factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset");
    led_music_override(SIGNAL_COLOR);
    status_led_override(1);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    esp_wifi_restore(); /* home network and the driver's copy of the AP config */

    vTaskDelay(pdMS_TO_TICKS(1000)); /* keep the "done" light on for a moment */
    esp_restart();
}

static void button_task(void *arg)
{
    uint32_t held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(BOARD_BOOT_BUTTON) != 0) {
            if (held_ms >= FEEDBACK_MS) {
                ESP_LOGI(TAG, "Released, no reset");
                led_music_override(LED_MUSIC_NO_OVERRIDE);
                status_led_override(-1);
            }
            held_ms = 0;
            continue;
        }

        held_ms += POLL_MS;
        if (held_ms == FEEDBACK_MS) {
            ESP_LOGI(TAG, "Hold for %d s more to reset to factory settings", (RESET_MS - FEEDBACK_MS) / 1000);
        }
        if (held_ms >= RESET_MS) {
            factory_reset();
        }
        if (held_ms >= FEEDBACK_MS) {
            bool on = (held_ms / BLINK_MS) % 2;
            led_music_override(on ? SIGNAL_COLOR : 0x000000);
            status_led_override(on);
        }
    }
}

void button_start(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
}
