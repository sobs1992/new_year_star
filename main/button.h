#pragma once

/* BOOT button held for 5 s while running: factory reset (settings, home Wi-Fi, access point
 * password; uploaded tracks stay), then reboot. Needs wifi_start() done. */
void button_start(void);
