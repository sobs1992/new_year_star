#pragma once

#include "freertos/FreeRTOS.h"

/* Tests run in one thread: a mutex only has to exist */
typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int mutex;
    return &mutex;
}

static inline int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    return pdTRUE;
}

static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{
    return pdTRUE;
}
