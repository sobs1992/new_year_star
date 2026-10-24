#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Uploading and deleting the playlist's MP3 files: the file in storage plus its entry in the
 * track list of the settings. Only the httpd task calls these. */

/* An upload in steps: begin checks the name, the track count and the free space, write takes
 * the file piece by piece (it must start like an MP3 file), finish adds it to the track list.
 * A failed step ends the upload and puts a message for the web page into msg. Errors:
 * ESP_ERR_INVALID_STATE the name is taken, ESP_ERR_INVALID_SIZE the file does not fit, any
 * other: a bad request. */
esp_err_t tracks_upload_begin(const char *name, size_t size, char *msg, size_t msg_len);
esp_err_t tracks_upload_write(const void *data, size_t len, char *msg, size_t msg_len);
esp_err_t tracks_upload_finish(char *msg, size_t msg_len);
/* The connection broke: drop what was received */
void tracks_upload_abort(void);

/* Drop the track from the list (the player stops it), then its file. ESP_ERR_NOT_FOUND: there
 * is no such track. */
esp_err_t tracks_delete(const char *name);
