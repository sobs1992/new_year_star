#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* The fields storage.c sets */
typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
/* 5 MB with 1 MB used */
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
