#pragma once

#include <stddef.h>

/* Test side of the stubs */

/* NVS: put a blob (a string, stored with its '\0' like settings.c does), read one back
 * (NULL when missing), count blob writes */
void stub_nvs_put(const char *ns, const char *key, const char *value);
const char *stub_nvs_get(const char *ns, const char *key);
extern int stub_nvs_writes;

/* Directory listing of STORAGE_BASE_PATH (opendir / readdir are wrapped at link time).
 * A name ending in '/' is a directory. Without a call the directory cannot be opened. */
void stub_storage_files(const char *const *names, size_t count);
