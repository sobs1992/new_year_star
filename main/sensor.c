#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"
#include "board.h"
#include "sensor.h"
#include "settings.h"

/* mmWave for XIAO pins: its TX sits on D2, its RX on D3 */
#define SENSOR_UART         UART_NUM_1
#define SENSOR_BAUD         256000

#define OFFLINE_MS          2000  /* radar reports ~10 frames/s */
#define ACK_TIMEOUT_MS      500
#define APPLY_RETRY_MS      5000
#define RECOVER_MS          5000  /* silent this long: restart the radar, again after as long */
#define NO_ONE_DURATION_S   5     /* radar's hold time before "no one" (factory value); with less,
                                     a sitting person flickers between present and absent */
#define MAX_FRAME           64
#define GATES               9     /* distance gates 0..8, 0.75 m each */
#define DISTANCE_WINDOW     15    /* frames in the distance median, ~1.5 s: the distance of a
                                     still target is noisy, single frames jump by meters */
#define SENSITIVITY_FACTORY 40    /* setting value that means "factory thresholds" */

/* Commands (all little-endian) */
#define CMD_ENABLE_CONFIG   0x00FF
#define CMD_END_CONFIG      0x00FE
#define CMD_SET_GATES       0x0060
#define CMD_SET_SENSITIVITY 0x0064
#define CMD_READ_PARAMS     0x0061
#define CMD_RESTART         0x00A3

/* Factory energy thresholds per gate (user manual, table 7). Near gates are noisier, so the
 * radar is less sensitive there; still thresholds of gates 0-1 cannot be set. */
static const uint8_t FACTORY_MOVING[GATES] = { 50, 50, 40, 30, 20, 15, 15, 15, 15 };
static const uint8_t FACTORY_STILL[GATES] = { 0, 0, 40, 40, 30, 30, 20, 20, 20 };

static const char *TAG = "sensor";

static const uint8_t DATA_HEAD[4] = { 0xF4, 0xF3, 0xF2, 0xF1 };
static const uint8_t DATA_TAIL[4] = { 0xF8, 0xF7, 0xF6, 0xF5 };
static const uint8_t CMD_HEAD[4] = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t CMD_TAIL[4] = { 0x04, 0x03, 0x02, 0x01 };
static const uint8_t ENABLE_CONFIG_VALUE[2] = { 0x01, 0x00 };

typedef enum {
    FRAME_NONE,
    FRAME_DATA,
    FRAME_ACK,
} frame_type_t;

typedef struct {
    uint8_t buf[MAX_FRAME];
    size_t len;
} parser_t;

/* What the radar holds in its flash */
typedef struct {
    uint8_t max_moving_gate;
    uint8_t max_still_gate;
    uint8_t moving[GATES]; /* energy thresholds */
    uint8_t still[GATES];
    uint16_t no_one_s;
} radar_params_t;

static parser_t parser;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static sensor_state_t state;
static TickType_t last_frame;
static uint16_t distances[DISTANCE_WINDOW];
static size_t distance_pos;
static uint8_t ack_payload[MAX_FRAME]; /* last matching ACK, for commands that return data */
static uint16_t ack_len;
static atomic_uint restarts; /* of the radar when it went silent */

static int compare_u16(const void *a, const void *b)
{
    return *(const uint16_t *)a - *(const uint16_t *)b;
}

/* Feed one byte; on a complete frame returns its type and points payload into the parser buffer,
 * valid until the next call */
static frame_type_t parser_feed(parser_t *p, uint8_t byte, const uint8_t **payload, uint16_t *payload_len)
{
    p->buf[p->len++] = byte;

    /* Resync: buffer must start with (a prefix of) a known header */
    while (p->len > 0) {
        size_t n = p->len < 4 ? p->len : 4;
        if (memcmp(p->buf, DATA_HEAD, n) == 0 || memcmp(p->buf, CMD_HEAD, n) == 0) {
            break;
        }
        memmove(p->buf, p->buf + 1, --p->len);
    }
    if (p->len < 6) {
        return FRAME_NONE;
    }

    uint16_t data_len = p->buf[4] | p->buf[5] << 8;
    size_t total = 4 + 2 + data_len + 4;
    if (total > sizeof(p->buf)) {
        p->len = 0; /* garbage length */
        return FRAME_NONE;
    }
    if (p->len < total) {
        return FRAME_NONE;
    }

    bool is_data = p->buf[0] == DATA_HEAD[0];
    p->len = 0;
    if (memcmp(p->buf + 6 + data_len, is_data ? DATA_TAIL : CMD_TAIL, 4) != 0) {
        return FRAME_NONE;
    }
    *payload = p->buf + 6;
    *payload_len = data_len;
    return is_data ? FRAME_DATA : FRAME_ACK;
}

/* Basic target info, same offsets in normal and engineering mode:
 * type, 0xAA, state, moving dist(2), moving energy, still dist(2), still energy, detect dist(2) */
static void handle_data(const uint8_t *d, uint16_t len)
{
    if (len < 13 || d[1] != 0xAA) {
        return;
    }
    uint8_t target = d[2] & 0x03;
    uint16_t moving_cm = d[3] | d[4] << 8;
    uint16_t still_cm = d[6] | d[7] << 8;

    /* Distance of what is detected, median-filtered. The moving distance is accurate, so it
     * is preferred; the still one is used only when nothing moves. */
    uint16_t median = 0;
    if (target != SENSOR_TARGET_NONE) {
        distances[distance_pos++ % DISTANCE_WINDOW] = target & SENSOR_TARGET_MOVING ? moving_cm : still_cm;
        size_t n = distance_pos < DISTANCE_WINDOW ? distance_pos : DISTANCE_WINDOW;
        uint16_t sorted[DISTANCE_WINDOW];
        memcpy(sorted, distances, n * sizeof(sorted[0]));
        qsort(sorted, n, sizeof(sorted[0]), compare_u16);
        median = sorted[n / 2];
    } else {
        distance_pos = 0;
    }

    portENTER_CRITICAL(&state_lock);
    state.target = target;
    state.moving_cm = moving_cm;
    state.still_cm = still_cm;
    state.moving_energy = d[5];
    state.still_energy = d[8];
    state.distance_cm = median;
    last_frame = xTaskGetTickCount();
    portEXIT_CRITICAL(&state_lock);
}

/* Read and dispatch incoming bytes; returns true when the expected ACK arrives (ack_cmd != 0) */
static bool pump(int timeout_ms, uint16_t ack_cmd, bool *ack_ok)
{
    uint8_t rx[64];
    int n = uart_read_bytes(SENSOR_UART, rx, sizeof(rx), pdMS_TO_TICKS(timeout_ms));
    for (int i = 0; i < n; i++) {
        const uint8_t *payload;
        uint16_t len;
        frame_type_t type = parser_feed(&parser, rx[i], &payload, &len);
        if (type == FRAME_DATA) {
            handle_data(payload, len);
        } else if (type == FRAME_ACK && ack_cmd && len >= 4 &&
                   (payload[0] | payload[1] << 8) == (ack_cmd | 0x0100)) {
            *ack_ok = (payload[2] | payload[3] << 8) == 0;
            ack_len = len;
            memcpy(ack_payload, payload, len);
            return true;
        }
    }
    return false;
}

static bool send_command(uint16_t cmd, const uint8_t *value, uint16_t value_len)
{
    uint8_t frame[4 + 2 + 2 + 32 + 4];
    uint16_t data_len = 2 + value_len;
    memcpy(frame, CMD_HEAD, 4);
    frame[4] = data_len & 0xFF;
    frame[5] = data_len >> 8;
    frame[6] = cmd & 0xFF;
    frame[7] = cmd >> 8;
    if (value_len) {
        memcpy(frame + 8, value, value_len);
    }
    memcpy(frame + 8 + value_len, CMD_TAIL, 4);
    uart_write_bytes(SENSOR_UART, frame, 8 + value_len + 4);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_TIMEOUT_MS);
    bool ok = false;
    while (xTaskGetTickCount() < deadline) {
        if (pump(20, cmd, &ok)) {
            return ok;
        }
    }
    return false;
}

static void put_word_param(uint8_t *p, uint16_t word, uint32_t value)
{
    p[0] = word & 0xFF;
    p[1] = word >> 8;
    p[2] = value & 0xFF;
    p[3] = (value >> 8) & 0xFF;
    p[4] = (value >> 16) & 0xFF;
    p[5] = value >> 24;
}

/* The setting scales the factory profile: 40 keeps it, lower is more sensitive everywhere */
static uint32_t scaled_threshold(uint8_t factory, uint8_t sensitivity)
{
    uint32_t t = (factory * sensitivity + SENSITIVITY_FACTORY / 2) / SENSITIVITY_FACTORY;
    return t > 100 ? 100 : t;
}

static void wanted_params(const sensor_settings_t *s, radar_params_t *out)
{
    out->max_moving_gate = s->max_gate;
    out->max_still_gate = s->max_gate;
    out->no_one_s = NO_ONE_DURATION_S;
    for (int g = 0; g < GATES; g++) {
        out->moving[g] = scaled_threshold(FACTORY_MOVING[g], s->sensitivity);
        out->still[g] = scaled_threshold(FACTORY_STILL[g], s->sensitivity);
    }
}

static bool params_equal(const radar_params_t *a, const radar_params_t *b)
{
    if (a->max_moving_gate != b->max_moving_gate || a->max_still_gate != b->max_still_gate ||
        a->no_one_s != b->no_one_s) {
        return false;
    }
    for (int g = 0; g < GATES; g++) {
        /* still thresholds of gates 0-1 cannot be set, whatever the radar reports there */
        if (a->moving[g] != b->moving[g] || (g >= 2 && a->still[g] != b->still[g])) {
            return false;
        }
    }
    return true;
}

/* From the read-parameters ACK (config mode only):
 * status(2) 0xAA, N, max moving gate, max still gate, moving[0..N], still[0..N], no-one duration(2) */
static bool read_radar_params(radar_params_t *out)
{
    if (!send_command(CMD_READ_PARAMS, NULL, 0) || ack_len < 8 || ack_payload[4] != 0xAA) {
        return false;
    }
    const uint8_t *p = ack_payload + 5;
    int n = p[0] + 1;
    if (n < GATES || ack_len < 5 + 3 + 2 * n + 2) {
        return false;
    }
    out->max_moving_gate = p[1];
    out->max_still_gate = p[2];
    memcpy(out->moving, p + 3, GATES);
    memcpy(out->still, p + 3 + n, GATES);
    out->no_one_s = p[3 + 2 * n] | p[4 + 2 * n] << 8;
    return true;
}

static void log_radar_params(const radar_params_t *p)
{
    char moving[GATES * 4 + 1] = "", still[GATES * 4 + 1] = "";
    for (int g = 0; g < GATES; g++) {
        snprintf(moving + strlen(moving), sizeof(moving) - strlen(moving), " %d", p->moving[g]);
        snprintf(still + strlen(still), sizeof(still) - strlen(still), " %d", p->still[g]);
    }
    ESP_LOGI(TAG, "Radar: max gates moving %d still %d, no-one %d s",
             p->max_moving_gate, p->max_still_gate, p->no_one_s);
    ESP_LOGI(TAG, "Radar thresholds moving:%s", moving);
    ESP_LOGI(TAG, "Radar thresholds still: %s", still);
}

/* Config mode only */
static bool write_radar_params(const radar_params_t *p)
{
    uint8_t gates[18];
    put_word_param(gates, 0x0000, p->max_moving_gate);
    put_word_param(gates + 6, 0x0001, p->max_still_gate);
    put_word_param(gates + 12, 0x0002, p->no_one_s);
    bool ok = send_command(CMD_SET_GATES, gates, sizeof(gates));
    for (int g = 0; ok && g < GATES; g++) {
        uint8_t sens[18];
        put_word_param(sens, 0x0000, g);
        put_word_param(sens + 6, 0x0001, p->moving[g]);
        /* gates 0-1 have no settable still threshold, the radar ignores the value */
        put_word_param(sens + 12, 0x0002, p->still[g]);
        ok = send_command(CMD_SET_SENSITIVITY, sens, sizeof(sens));
        if (!ok) {
            ESP_LOGW(TAG, "Radar rejected thresholds of gate %d", g);
        }
    }
    return ok;
}

/* Stored in the radar's flash, so only written when it holds something else. The radar sends
 * no data in config mode, so that is kept short: reading takes 3 commands, writing 13. */
static bool apply_radar_settings(const sensor_settings_t *s)
{
    radar_params_t want, have;
    wanted_params(s, &want);

    bool ok = send_command(CMD_ENABLE_CONFIG, ENABLE_CONFIG_VALUE, sizeof(ENABLE_CONFIG_VALUE));
    if (ok) {
        if (read_radar_params(&have) && params_equal(&have, &want)) {
            ESP_LOGI(TAG, "Radar holds these settings already");
        } else {
            ok = write_radar_params(&want);
            if (read_radar_params(&have)) {
                log_radar_params(&have);
            } else {
                ESP_LOGW(TAG, "Radar parameters could not be read back");
            }
        }
    }
    /* Also when the ACK above got lost: the radar may be in config mode all the same */
    send_command(CMD_END_CONFIG, NULL, 0);
    return ok;
}

/* The radar stays in config mode, silent, until END_CONFIG, which never comes when the ESP
 * restarts midway; a restart brings it back from that, and from whatever else it hangs in */
static bool restart_radar(void)
{
    /* Its result does not matter: the radar may be in config mode already */
    send_command(CMD_ENABLE_CONFIG, ENABLE_CONFIG_VALUE, sizeof(ENABLE_CONFIG_VALUE));
    return send_command(CMD_RESTART, NULL, 0);
}

static void sensor_task(void *arg)
{
    uint32_t applied = UINT32_MAX; /* push settings once after boot, then on change */
    TickType_t last_try = 0;
    TickType_t heard = xTaskGetTickCount(); /* last seen online, or the last restart */
    bool was_online = false;

    for (;;) {
        bool dummy;
        pump(50, 0, &dummy);

        sensor_state_t s;
        sensor_get_state(&s);
        if (s.online != was_online) {
            ESP_LOGI(TAG, "%s", s.online ? "Radar online" : "Radar offline, check wiring (D2/D3, 3V3)");
            was_online = s.online;
        }

        TickType_t now = xTaskGetTickCount();
        if (s.online) {
            heard = now;
        } else if (now - heard >= pdMS_TO_TICKS(RECOVER_MS)) {
            heard = now;
            /* No ACK: no radar on the wires, or it hangs beyond commands; nothing to log again */
            if (restart_radar()) {
                atomic_fetch_add(&restarts, 1);
                ESP_LOGW(TAG, "Radar was silent, restarted it");
            }
        }

        uint32_t version = settings_radar_version();
        if (s.online && version != applied && now - last_try >= pdMS_TO_TICKS(APPLY_RETRY_MS)) {
            last_try = now;
            sensor_settings_t cfg;
            settings_get_sensor(&cfg);
            if (apply_radar_settings(&cfg)) {
                applied = version;
                ESP_LOGI(TAG, "Radar set: up to %d.%02d m, threshold %d",
                         cfg.max_gate * 75 / 100, cfg.max_gate * 75 % 100, cfg.sensitivity);
            } else {
                ESP_LOGW(TAG, "Radar did not accept settings, will retry");
            }
        }
    }
}

void sensor_start(void)
{
    uart_config_t cfg = {
        .baud_rate = SENSOR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SENSOR_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SENSOR_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SENSOR_UART, BOARD_RADAR_TX, BOARD_RADAR_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART%d %d baud, RX GPIO%d (D2), TX GPIO%d (D3)",
             SENSOR_UART, SENSOR_BAUD, BOARD_RADAR_RX, BOARD_RADAR_TX);
}

void sensor_get_state(sensor_state_t *out)
{
    portENTER_CRITICAL(&state_lock);
    *out = state;
    out->online = last_frame != 0 && xTaskGetTickCount() - last_frame < pdMS_TO_TICKS(OFFLINE_MS);
    portEXIT_CRITICAL(&state_lock);
    if (!out->online) {
        out->target = SENSOR_TARGET_NONE;
    }
}

bool sensor_triggered(uint8_t trigger)
{
    sensor_state_t s;
    sensor_get_state(&s);
    if (trigger == SENSOR_TRIGGER_MOTION) {
        return s.target & SENSOR_TARGET_MOVING;
    }
    return s.target != SENSOR_TARGET_NONE;
}

static const char *target_name(sensor_target_t t)
{
    switch (t) {
    case SENSOR_TARGET_NONE: return "none";
    case SENSOR_TARGET_MOVING: return "moving";
    case SENSOR_TARGET_STILL: return "still";
    case SENSOR_TARGET_BOTH: return "both";
    }
    return "unknown";
}

void sensor_status_json(cJSON *out)
{
    sensor_state_t s;
    sensor_get_state(&s);
    cJSON_AddBoolToObject(out, "online", s.online);
    cJSON_AddStringToObject(out, "target", target_name(s.target));
    cJSON_AddNumberToObject(out, "distance_cm", s.distance_cm);
    cJSON_AddNumberToObject(out, "moving_cm", s.moving_cm);
    cJSON_AddNumberToObject(out, "still_cm", s.still_cm);
    cJSON_AddNumberToObject(out, "moving_energy", s.moving_energy);
    cJSON_AddNumberToObject(out, "still_energy", s.still_energy);
    cJSON_AddNumberToObject(out, "restarts", atomic_load(&restarts));
}
