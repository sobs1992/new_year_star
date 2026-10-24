#pragma once

#include <stdint.h>

/* Yellow user LED on the board: steady on when all is fine, otherwise blinks the code of the
 * most important error (fewest blinks), pause, repeat. */

typedef enum {
    STATUS_ERR_STORAGE = 1, /* track storage did not mount */
    STATUS_ERR_WIFI,        /* home network is set but cannot be joined */
    STATUS_ERR_RADIO,       /* radio mode: the stream fails */
    STATUS_ERR_SENSOR,      /* motion sensor enabled but the radar does not answer */
    STATUS_ERR_NOTHING,     /* nothing to play: no enabled track, or no station */
    STATUS_ERR_MAX = STATUS_ERR_NOTHING,
} status_error_t;

void status_start(void);

struct cJSON;
/* "errors" of /api/status: the codes of the active errors, an array */
void status_errors_json(struct cJSON *array);

/* Let another module drive the LED (factory reset countdown): 0 off, 1 on, -1 give it back */
void status_led_override(int state);
