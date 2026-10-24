#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "storage.h"
#include "stubs.h"

/* ---------- log, random, errors ---------- */

void stub_log(char level, const char *tag, const char *fmt, ...)
{
    static int verbose = -1;
    if (verbose < 0) {
        const char *v = getenv("VERBOSE");
        verbose = v && v[0] && strcmp(v, "0") != 0;
    }
    if (!verbose) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "    %c %s: ", level, tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

uint32_t esp_random(void)
{
    return 0x5eed;
}

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

/* ---------- NVS ---------- */

#define NVS_ENTRIES 8
#define NVS_HANDLES 4

static struct {
    char ns[16];
    char key[16];
    char *data;
    size_t len;
} entries[NVS_ENTRIES];

static char handles[NVS_HANDLES][16]; /* namespace by handle - 1, empty when closed */

int stub_nvs_writes;

static int find_entry(const char *ns, const char *key)
{
    for (int i = 0; i < NVS_ENTRIES; i++) {
        if (entries[i].data && strcmp(entries[i].ns, ns) == 0 && strcmp(entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static void put(const char *ns, const char *key, const void *value, size_t len)
{
    int i = find_entry(ns, key);
    for (int j = 0; i < 0 && j < NVS_ENTRIES; j++) {
        if (entries[j].data == NULL) {
            i = j;
        }
    }
    if (i < 0) {
        abort(); /* more blobs than a test needs */
    }
    free(entries[i].data);
    strncpy(entries[i].ns, ns, sizeof(entries[i].ns) - 1);
    strncpy(entries[i].key, key, sizeof(entries[i].key) - 1);
    entries[i].data = malloc(len);
    memcpy(entries[i].data, value, len);
    entries[i].len = len;
}

void stub_nvs_put(const char *ns, const char *key, const char *value)
{
    put(ns, key, value, strlen(value) + 1);
}

const char *stub_nvs_get(const char *ns, const char *key)
{
    int i = find_entry(ns, key);
    return i < 0 ? NULL : entries[i].data;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    bool exists = false;
    for (int i = 0; i < NVS_ENTRIES; i++) {
        exists |= entries[i].data && strcmp(entries[i].ns, ns) == 0;
    }
    if (mode == NVS_READONLY && !exists) {
        return ESP_ERR_NVS_NOT_FOUND; /* as the real one: the namespace is created by a write */
    }
    for (int h = 0; h < NVS_HANDLES; h++) {
        if (handles[h][0] == '\0') {
            strncpy(handles[h], ns, sizeof(handles[h]) - 1);
            *out = h + 1;
            return ESP_OK;
        }
    }
    abort(); /* handle leak */
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    int i = find_entry(handles[h - 1], key);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out) {
        if (*len < entries[i].len) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(out, entries[i].data, entries[i].len);
    }
    *len = entries[i].len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    put(handles[h - 1], key, value, len);
    stub_nvs_writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    handles[h - 1][0] = '\0';
}

/* ---------- LittleFS ---------- */

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    return ESP_OK;
}

esp_err_t esp_littlefs_info(const char *partition_label, size_t *total, size_t *used)
{
    *total = 5 * 1024 * 1024;
    *used = 1024 * 1024;
    return ESP_OK;
}

/* ---------- the track directory, linked with --wrap=opendir,readdir,closedir,stat ---------- */

int __real_stat(const char *path, struct stat *st);

/* A file in STORAGE_BASE_PATH is 100000 bytes plus the length of its name */
int __wrap_stat(const char *path, struct stat *st)
{
    const char *dir = STORAGE_BASE_PATH "/";
    if (strncmp(path, dir, strlen(dir)) != 0) {
        return __real_stat(path, st);
    }
    memset(st, 0, sizeof(*st));
    st->st_size = 100000 + strlen(path + strlen(dir));
    return 0;
}

static const char *const *files;
static size_t file_count, file_pos;
static struct dirent entry;
static bool listed;

void stub_storage_files(const char *const *names, size_t count)
{
    files = names;
    file_count = count;
    listed = true;
}

DIR *__wrap_opendir(const char *path)
{
    if (!listed || strcmp(path, STORAGE_BASE_PATH) != 0) {
        return NULL;
    }
    file_pos = 0;
    return (DIR *)&entry; /* never dereferenced by the caller */
}

struct dirent *__wrap_readdir(DIR *dir)
{
    if (file_pos == file_count) {
        return NULL;
    }
    const char *name = files[file_pos++];
    size_t len = strlen(name);
    bool is_dir = len > 0 && name[len - 1] == '/';
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.d_name, name, is_dir ? len - 1 : len);
    entry.d_type = is_dir ? DT_DIR : DT_REG;
    return &entry;
}

int __wrap_closedir(DIR *dir)
{
    return 0;
}
