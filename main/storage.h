#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* LittleFS on the "storage" partition with the MP3 tracks */
#define STORAGE_BASE_PATH   "/tracks"
/* Upload goes here first and is renamed when complete; hidden from the track list */
#define STORAGE_UPLOAD_TMP  STORAGE_BASE_PATH "/.upload"
#define TRACK_NAME_SIZE     64  /* bytes incl. '\0', LittleFS name limit */

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

/* Acceptable track file name: "*.mp3", not hidden, fits TRACK_NAME_SIZE */
bool storage_is_track_name(const char *name);

/* Call found() with the name of every track file, in directory order; false if the directory
 * cannot be read */
bool storage_for_each_track(void (*found)(const char *name, void *ctx), void *ctx);
