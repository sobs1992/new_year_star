#pragma once

/* System page data: CPU load per task, internal RAM, PSRAM, chip temperature, versions */

/* Start the chip temperature sensor */
void sysinfo_start(void);

struct cJSON;
/* "heap" of /api/status: free internal RAM (now, largest block, minimum since boot), free PSRAM */
void sysinfo_heap_json(struct cJSON *out);

/* JSON for /api/system. CPU load covers the time since the previous call (at least 1 s, a
 * sooner call gets the same numbers). Only the httpd task may call it. Caller frees. */
char *sysinfo_json(void);
