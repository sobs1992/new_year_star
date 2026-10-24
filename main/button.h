#pragma once

#include <stdint.h>

/* BOOT button held for 5 s while running: factory reset (settings, home Wi-Fi, access point
 * password; uploaded tracks stay), then reboot. Needs wifi_start() done. */

void button_init(void);

/* The status task calls this every elapsed_ms: the button has no task of its own, which saves
 * its stack in internal RAM */
void button_poll(uint32_t elapsed_ms);
