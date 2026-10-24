#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocations of cspot, nanopb and Tremor: PSRAM first, internal RAM only when PSRAM is full.
 * The default policy keeps blocks under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL internal, and a
 * Spotify session makes many of them; the star's internal RAM is needed for Wi-Fi and DMA.
 * Free with free(). */
void *cspot_malloc(size_t size);
void *cspot_calloc(size_t n, size_t size);
void *cspot_realloc(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif
