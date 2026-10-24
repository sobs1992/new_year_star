#include <stdatomic.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "board.h"
#include "button.h"
#include "player.h"
#include "radio.h"
#include "sensor.h"
#include "settings.h"
#include "status.h"
#include "storage.h"
#include "wifi.h"

#define TICK_MS         50
#define BLINK_ON_MS     200
#define BLINK_OFF_MS    300
#define CODE_PAUSE_MS   1500        /* between repetitions of a code */
#define SENSOR_GRACE_MS 5000        /* the radar needs a moment after boot */

static const char *TAG = "status";

static atomic_uint_fast32_t errors;
static atomic_int override = -1;

static void led(bool on)
{
    gpio_set_level(BOARD_STATUS_LED, !on);
}

static uint32_t check_errors(void)
{
    uint32_t e = 0;
    if (!storage_mounted()) {
        e |= 1 << STATUS_ERR_STORAGE;
    }

    wifi_status_t ws;
    wifi_get_status(&ws);
    if (ws.state == WIFI_STA_FAILED) {
        e |= 1 << STATUS_ERR_WIFI;
    }

    player_status_t ps;
    char track[TRACK_NAME_SIZE];
    player_get_status(&ps, track);
    radio_status_t rs;
    radio_get_status(&rs);
    if (ps == PLAYER_RADIO && rs.state == RADIO_ERROR) {
        e |= 1 << STATUS_ERR_RADIO;
    }
    if (ps == PLAYER_IDLE) {
        e |= 1 << STATUS_ERR_NOTHING;
    }

    sensor_settings_t ss;
    settings_get_sensor(&ss);
    sensor_state_t sensor;
    sensor_get_state(&sensor);
    if (ss.enabled && !sensor.online && esp_timer_get_time() / 1000 > SENSOR_GRACE_MS) {
        e |= 1 << STATUS_ERR_SENSOR;
    }
    return e;
}

/* Lowest code first: it is the most basic problem */
static int top_error(uint32_t e)
{
    for (int code = 1; code <= STATUS_ERR_MAX; code++) {
        if (e & 1 << code) {
            return code;
        }
    }
    return 0;
}

static void status_task(void *arg)
{
    int shown = 0;      /* code in the current blink sequence */
    uint32_t phase = 0; /* ms into the sequence */

    for (;;) {
        button_poll(TICK_MS); /* may override the LED */
        uint32_t e = check_errors();
        uint32_t old = atomic_exchange(&errors, e);
        if (e != old) {
            ESP_LOGI(TAG, "Errors: 0x%02lx%s", (unsigned long)e, e ? "" : " (all fine)");
        }

        int forced = atomic_load(&override);
        if (forced >= 0) {
            led(forced);
        } else {
            /* A new code waits for the current sequence to finish, so blinks are never cut */
            int code = top_error(e);
            uint32_t length = shown * (BLINK_ON_MS + BLINK_OFF_MS) + CODE_PAUSE_MS;
            if (shown == 0 || phase >= length) {
                shown = code;
                phase = 0;
            }
            if (shown == 0) {
                led(true);
            } else {
                uint32_t in_blink = phase % (BLINK_ON_MS + BLINK_OFF_MS);
                bool blinking = phase < shown * (BLINK_ON_MS + BLINK_OFF_MS);
                led(blinking && in_blink < BLINK_ON_MS);
                phase += TICK_MS;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void status_start(void)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << BOARD_STATUS_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    led(true);
    /* Priority of the button it polls: a hold must be seen under load too */
    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);
}

void status_errors_json(cJSON *array)
{
    uint32_t e = atomic_load(&errors);
    for (int code = 1; code <= STATUS_ERR_MAX; code++) {
        if (e & 1 << code) {
            cJSON_AddItemToArray(array, cJSON_CreateNumber(code));
        }
    }
}

void status_led_override(int state)
{
    atomic_store(&override, state);
}
