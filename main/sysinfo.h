#pragma once

/* System page data: CPU load per task, internal RAM, PSRAM, chip temperature, versions */

/* Start the chip temperature sensor */
void sysinfo_start(void);

/* JSON for /api/system. CPU load covers the time since the previous call (at least 1 s, a
 * sooner call gets the same numbers). Only the httpd task may call it. Caller frees. */
char *sysinfo_json(void);
