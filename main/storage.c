#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "storage.h"

#define PARTITION_LABEL "storage"
#define UPLOAD_RESERVE  (32 * 1024) /* LittleFS metadata and block rounding */

static const char *TAG = "storage";

static bool mounted;

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true, /* blank partition on first boot */
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    mounted = true;
    unlink(STORAGE_UPLOAD_TMP); /* left by an upload interrupted by power loss */
    size_t total, used;
    storage_info(&total, &used);
    ESP_LOGI(TAG, "Mounted at %s: %u of %u KB used", STORAGE_BASE_PATH,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

bool storage_mounted(void)
{
    return mounted;
}

void storage_info(size_t *total, size_t *used)
{
    if (esp_littlefs_info(PARTITION_LABEL, total, used) != ESP_OK) {
        *total = 0;
        *used = 0;
    }
}

size_t storage_upload_space(void)
{
    size_t total, used;
    storage_info(&total, &used);
    return total > used + UPLOAD_RESERVE ? total - used - UPLOAD_RESERVE : 0;
}

void storage_path(const char *name, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/%s", STORAGE_BASE_PATH, name);
}

size_t storage_file_size(const char *name)
{
    char path[128];
    struct stat st;
    storage_path(name, path, sizeof(path));
    return stat(path, &st) == 0 ? st.st_size : 0;
}
