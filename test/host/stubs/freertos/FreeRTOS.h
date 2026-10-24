#pragma once

/* Just enough FreeRTOS for single-threaded host tests */

#include <assert.h>
#include <stdint.h>

typedef uint32_t TickType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdTRUE  1
#define pdFALSE 0
