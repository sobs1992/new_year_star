#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* LittleFS on the "storage" partition with the MP3 tracks */
#define STORAGE_BASE_PATH   "/tracks"
/* Upload goes here first and is renamed when complete; hidden from the track list */
#define STORAGE_UPLOAD_TMP  STORAGE_BASE_PATH "/.upload"

esp_err_t storage_init(void);
bool storage_mounted(void);

/* Bytes in the filesystem */
void storage_info(size_t *total, size_t *used);

/* Full path for a file name in the storage */
void storage_path(const char *name, char *path, size_t path_len);

/* Largest file that can be uploaded now: free space minus room for LittleFS metadata */
size_t storage_upload_space(void);

/* Size of a stored file, 0 when missing */
size_t storage_file_size(const char *name);
