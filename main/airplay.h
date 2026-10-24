#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AirPlay 1 (RAOP) receiver: iPhones and Macs stream ALAC / PCM audio over RTP */

void airplay_start(void);

/* Switched off, it is not announced, refuses senders and ends a session within a second; may
 * come before airplay_start() */
void airplay_set_enabled(bool on);

/* A sender is streaming to us */
bool airplay_active(void);

/* Up to max_frames stereo 16-bit frames at 44.1 kHz, in playback order; waits up to
 * timeout_ms for the jitter buffer. Returns 0 when nothing is due (a gap is silence). */
size_t airplay_read(int16_t *pcm, size_t max_frames, int timeout_ms);

/* Set by the sender: 0..100 */
uint8_t airplay_volume(void);

/* Stop button on the web page: drop the session */
void airplay_stop(void);

struct cJSON;
/* "airplay" of /api/status: packet counters of the current / last session, volume */
void airplay_status_json(struct cJSON *out);
