#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Start the LED task that drives the WS2812 strip from the audio level */
void led_music_start(void);

/* Feed RMS levels (0..32767) of the PCM block that is about to be played: the whole signal
 * and its bass / mid / treble bands. All zero for silence. */
void led_music_set_levels(uint32_t total, uint32_t low, uint32_t mid, uint32_t high);

/* Pause after a track, waiting for the sensor or stopped: the glow goes to idle_brightness */
void led_music_set_idle(bool on);

#define LED_MUSIC_NO_OVERRIDE UINT32_MAX

/* Show a fixed 0xRRGGBB color instead of the light music (signals), or LED_MUSIC_NO_OVERRIDE */
void led_music_override(uint32_t rgb);
