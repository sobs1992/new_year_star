#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sysinfo.h"

#define MAX_TASKS     64
#define MIN_WINDOW_US 1000000 /* shortest CPU load window */

static const char *TAG = "sysinfo";

static temperature_sensor_handle_t tsens;

/* Run time counters at the start of the current window. In PSRAM (.bss there): internal RAM
 * is short, and only the httpd task touches these. */
typedef struct {
    TaskHandle_t handle;
    uint64_t runtime;
} task_mark_t;

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    uint16_t cpu_permille;
    uint8_t priority;
    bool stack_psram;
    uint32_t stack_free; /* bytes never used so far */
} task_load_t;

static EXT_RAM_BSS_ATTR TaskStatus_t status[MAX_TASKS];
static EXT_RAM_BSS_ATTR task_mark_t marks[MAX_TASKS];
static EXT_RAM_BSS_ATTR task_load_t loads[MAX_TASKS];
static size_t mark_count, load_count;
static uint64_t mark_total;    /* total run time at the window start */
static int64_t mark_at_us;
static uint16_t cpu_permille;  /* everything but the idle task */
static uint32_t window_ms;

static void update_load(void);

void sysinfo_start(void)
{
    update_load(); /* the first window starts now */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &tsens) != ESP_OK || temperature_sensor_enable(tsens) != ESP_OK) {
        ESP_LOGW(TAG, "No temperature sensor");
        tsens = NULL;
    }
}

static uint64_t previous_runtime(TaskHandle_t handle)
{
    for (size_t i = 0; i < mark_count; i++) {
        if (marks[i].handle == handle) {
            return marks[i].runtime;
        }
    }
    return 0; /* started during the window */
}

/* Close the load window if it is long enough and open the next one */
static void update_load(void)
{
    int64_t now = esp_timer_get_time();
    if (mark_at_us && now - mark_at_us < MIN_WINDOW_US) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(status, MAX_TASKS, &total);
    if (n == 0) {
        return; /* more than MAX_TASKS tasks */
    }

    uint64_t window = total - mark_total;
    TaskHandle_t idle = xTaskGetIdleTaskHandle();
    load_count = 0;
    for (UBaseType_t i = 0; i < n && window > 0; i++) {
        const TaskStatus_t *t = &status[i];
        uint64_t ran = t->ulRunTimeCounter - previous_runtime(t->xHandle);
        uint16_t permille = ran >= window ? 1000 : ran * 1000 / window;
        if (t->xHandle == idle) {
            cpu_permille = 1000 - permille;
        }
        task_load_t *l = &loads[load_count++];
        strlcpy(l->name, t->pcTaskName, sizeof(l->name));
        l->cpu_permille = permille;
        l->priority = t->uxCurrentPriority;
        l->stack_psram = esp_ptr_external_ram(t->pxStackBase);
        l->stack_free = t->usStackHighWaterMark * sizeof(StackType_t);
    }
    window_ms = window / 1000; /* the counter runs in microseconds (esp_timer) */

    for (UBaseType_t i = 0; i < n; i++) {
        marks[i] = (task_mark_t){ status[i].xHandle, status[i].ulRunTimeCounter };
    }
    mark_count = n;
    mark_total = total;
    mark_at_us = now;
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_EXT: return "external";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
    }
}

static void add_heap(cJSON *root, const char *name, uint32_t caps)
{
    cJSON *j = cJSON_AddObjectToObject(root, name);
    cJSON_AddNumberToObject(j, "total", heap_caps_get_total_size(caps));
    cJSON_AddNumberToObject(j, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(j, "min_free", heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(j, "largest", heap_caps_get_largest_free_block(caps));
}

char *sysinfo_json(void)
{
    update_load();
    cJSON *root = cJSON_CreateObject();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON *jchip = cJSON_AddObjectToObject(root, "chip");
    cJSON_AddStringToObject(jchip, "model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(jchip, "revision", chip.revision); /* major * 100 + minor */
    cJSON_AddNumberToObject(jchip, "cores", chip.cores);
    cJSON_AddNumberToObject(jchip, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(jchip, "flash", flash_size);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *japp = cJSON_AddObjectToObject(root, "app");
    cJSON_AddStringToObject(japp, "version", app->version);
    cJSON_AddStringToObject(japp, "idf", app->idf_ver);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(japp, "built", built);

    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(root, "reset_reason", reset_reason());

    float celsius;
    if (tsens && temperature_sensor_get_celsius(tsens, &celsius) == ESP_OK) {
        cJSON_AddNumberToObject(root, "temperature_c", (int)(celsius * 10) / 10.0);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
    }

    add_heap(root, "ram", MALLOC_CAP_INTERNAL);
    cJSON_AddNumberToObject(cJSON_GetObjectItem(root, "ram"), "dma_largest",
                            heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    add_heap(root, "psram", MALLOC_CAP_SPIRAM);

    cJSON *jcpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddNumberToObject(jcpu, "load", cpu_permille / 10.0);
    cJSON_AddNumberToObject(jcpu, "window_ms", window_ms);
    cJSON *jtasks = cJSON_AddArrayToObject(jcpu, "tasks");
    for (size_t i = 0; i < load_count; i++) {
        const task_load_t *l = &loads[i];
        cJSON *jt = cJSON_CreateObject();
        cJSON_AddStringToObject(jt, "name", l->name);
        cJSON_AddNumberToObject(jt, "cpu", l->cpu_permille / 10.0);
        cJSON_AddNumberToObject(jt, "priority", l->priority);
        cJSON_AddNumberToObject(jt, "stack_free", l->stack_free);
        cJSON_AddBoolToObject(jt, "stack_psram", l->stack_psram);
        cJSON_AddItemToArray(jtasks, jt);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
