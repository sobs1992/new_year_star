#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "player.h"
#include "settings.h"
#include "storage.h"
#include "tracks.h"

#define DELETE_WAIT_MS  2000 /* for the player to close the file */

static const char *TAG = "tracks";

/* The upload in progress: written to STORAGE_UPLOAD_TMP, renamed when complete */
static FILE *upload;
static char upload_name[TRACK_NAME_SIZE]; /* empty when there is none */
static size_t upload_received;

static bool looks_like_mp3(const uint8_t *d, size_t len)
{
    return len >= 3 && ((d[0] == 'I' && d[1] == 'D' && d[2] == '3') || /* ID3v2 tag */
                        (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0));    /* MPEG frame sync */
}

static esp_err_t upload_failed(esp_err_t err, const char *text, char *msg, size_t msg_len)
{
    tracks_upload_abort();
    strlcpy(msg, text, msg_len);
    return err;
}

esp_err_t tracks_upload_begin(const char *name, size_t size, char *msg, size_t msg_len)
{
    tracks_upload_abort(); /* one left unfinished */
    track_settings_t ts;
    if (!storage_is_track_name(name)) {
        snprintf(msg, msg_len, "Нужен файл .mp3 с именем не длиннее %d байт", TRACK_NAME_SIZE - 1);
        return ESP_ERR_INVALID_ARG;
    }
    if (settings_find_track(name, &ts)) {
        strlcpy(msg, "Файл с таким именем уже есть", msg_len);
        return ESP_ERR_INVALID_STATE;
    }
    if (settings_track_count() >= SETTINGS_MAX_TRACKS) {
        snprintf(msg, msg_len, "Не больше %d треков", SETTINGS_MAX_TRACKS);
        return ESP_ERR_NO_MEM;
    }
    size_t space = storage_upload_space();
    if (size == 0 || size > space) {
        snprintf(msg, msg_len, "Файл %u КБ не помещается, свободно %u КБ",
                 (unsigned)(size / 1024), (unsigned)(space / 1024));
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(upload_name, name, sizeof(upload_name));
    upload_received = 0;
    upload = fopen(STORAGE_UPLOAD_TMP, "wb");
    if (upload == NULL) {
        return upload_failed(ESP_FAIL, "Не удалось начать запись", msg, msg_len);
    }
    return ESP_OK;
}

esp_err_t tracks_upload_write(const void *data, size_t len, char *msg, size_t msg_len)
{
    if (upload_received == 0 && !looks_like_mp3(data, len)) {
        return upload_failed(ESP_FAIL, "Это не MP3-файл", msg, msg_len);
    }
    if (fwrite(data, 1, len, upload) != len) {
        return upload_failed(ESP_FAIL, "Ошибка записи во flash", msg, msg_len);
    }
    upload_received += len;
    return ESP_OK;
}

esp_err_t tracks_upload_finish(char *msg, size_t msg_len)
{
    fclose(upload);
    upload = NULL;
    char path[128];
    storage_path(upload_name, path, sizeof(path));
    if (rename(STORAGE_UPLOAD_TMP, path) != 0) {
        return upload_failed(ESP_FAIL, "Ошибка записи во flash", msg, msg_len);
    }
    if (settings_add_track(upload_name) != ESP_OK) {
        unlink(path);
        return upload_failed(ESP_FAIL, "Список треков переполнен", msg, msg_len);
    }
    ESP_LOGI(TAG, "Uploaded %s, %u bytes", upload_name, (unsigned)upload_received);
    upload_name[0] = '\0';
    return ESP_OK;
}

void tracks_upload_abort(void)
{
    if (upload) {
        fclose(upload);
        upload = NULL;
    }
    if (upload_name[0]) {
        unlink(STORAGE_UPLOAD_TMP);
        upload_name[0] = '\0';
    }
}

esp_err_t tracks_delete(const char *name)
{
    track_settings_t ts;
    if (!settings_find_track(name, &ts)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Dropping it from the list makes the player stop it on the next frame */
    settings_remove_track(name);
    for (int waited = 0; player_is_playing(name) && waited < DELETE_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    char path[128];
    storage_path(name, path, sizeof(path));
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "Delete of %s failed", name);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Deleted %s", name);
    return ESP_OK;
}
