#include "cspot_alloc.h"

#include "esp_heap_caps.h"

#define PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void *cspot_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, PSRAM, INTERNAL);
}

void *cspot_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, PSRAM, INTERNAL);
}

void *cspot_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2, PSRAM, INTERNAL);
}
