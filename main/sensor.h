#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Seeed 24GHz mmWave for XIAO (HLK-LD2410B) on UART */

typedef enum {
    SENSOR_TARGET_NONE = 0,
    SENSOR_TARGET_MOVING = 1,
    SENSOR_TARGET_STILL = 2,
    SENSOR_TARGET_BOTH = 3, /* moving and stationary */
} sensor_target_t;

typedef struct {
    bool online;          /* data frames received recently */
    uint8_t target;       /* sensor_target_t */
    uint16_t distance_cm; /* of the target, median of the last frames (the raw value jumps) */
    uint16_t moving_cm;   /* raw distances as the radar reports them */
    uint16_t still_cm;
    uint8_t moving_energy;
    uint8_t still_energy;
} sensor_state_t;

/* Start the task reading the radar; it also pushes gate/sensitivity settings to it */
void sensor_start(void);

void sensor_get_state(sensor_state_t *out);

/* Is there a target matching sensor_trigger_t right now */
bool sensor_triggered(uint8_t trigger);

struct cJSON;
/* "sensor" of /api/status */
void sensor_status_json(struct cJSON *out);
