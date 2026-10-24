#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AirPlay 1 (RAOP) receiver: iPhones and Macs stream ALAC / PCM audio over RTP */

void airplay_start(void);

/* A sender is streaming to us */
bool airplay_active(void);

/* Up to max_frames stereo 16-bit frames at 44.1 kHz, in playback order; waits up to
 * timeout_ms for the jitter buffer. Returns 0 when nothing is due (a gap is silence). */
size_t airplay_read(int16_t *pcm, size_t max_frames, int timeout_ms);

/* Set by the sender: 0..100 */
uint8_t airplay_volume(void);

typedef struct {
    uint32_t received; /* audio packets that decoded fine */
    uint32_t played;
    uint32_t lost;     /* played as silence: missing even after a resend request */
} airplay_stats_t;

/* Counters of the current / last session */
void airplay_get_stats(airplay_stats_t *out);

/* Stop button on the web page: drop the session */
void airplay_stop(void);
