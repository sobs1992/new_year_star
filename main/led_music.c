#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "led_music.h"
#include "settings.h"

#define LED_GPIO        GPIO_NUM_12 /* D7 */
#define FRAME_MS        10
#define PEAK_FLOOR      500.0f      /* don't amplify silence / noise */
#define BAND_FLOOR      100.0f      /* same for the band averages in RGB mode */
#define IDLE_FADE_MS    300         /* time constant of the glow change into / out of idle */
#define BAND_CONTRAST   5           /* power on band excitement: higher gives purer colors
                                       (on a test track: 3 - 24% white, 5 - ~10%, 6 - 6%) */

enum { LEVEL_TOTAL, LEVEL_LOW, LEVEL_MID, LEVEL_HIGH, LEVEL_COUNT };

static const char *TAG = "led_music";

static led_strip_handle_t strip;
static uint16_t strip_leds;
static atomic_uint_fast32_t levels[LEVEL_COUNT];
static atomic_uint_fast32_t override = LED_MUSIC_NO_OVERRIDE;
static atomic_bool idle;

void led_music_override(uint32_t rgb)
{
    atomic_store(&override, rgb);
}

void led_music_set_idle(bool on)
{
    atomic_store(&idle, on);
}

void led_music_set_levels(uint32_t total, uint32_t low, uint32_t mid, uint32_t high)
{
    atomic_store(&levels[LEVEL_TOTAL], total);
    atomic_store(&levels[LEVEL_LOW], low);
    atomic_store(&levels[LEVEL_MID], mid);
    atomic_store(&levels[LEVEL_HIGH], high);
}

/* Auto gain + smoothing for one level: brightness 0..1 */
typedef struct {
    float peak;
    float env;
} follower_t;

typedef struct {
    float attack;     /* one-pole smoothing: fraction of the gap to target closed per frame */
    float release;
    float peak_decay; /* auto gain: per-frame decay of the remembered peak */
    float avg_k;      /* per-frame weight of the slow band averages */
} dynamics_t;

static float follow(follower_t *f, float level, const dynamics_t *d)
{
    f->peak *= d->peak_decay;
    if (f->peak < PEAK_FLOOR) {
        f->peak = PEAK_FLOOR;
    }
    if (level > f->peak) {
        f->peak = level;
    }
    float norm = level / f->peak;
    f->env += (norm - f->env) * (norm > f->env ? d->attack : d->release);
    return f->env;
}

/* RGB mode: how much a band stands out against its own recent average */
typedef struct {
    float fast; /* smoothed like the brightness */
    float avg;  /* slow average, over the auto gain half-life */
} band_follower_t;

static float band_excitement(band_follower_t *b, float level, const dynamics_t *d)
{
    b->fast += (level - b->fast) * (level > b->fast ? d->attack : d->release);
    b->avg += (level - b->avg) * d->avg_k;
    return b->fast / (b->avg > BAND_FLOOR ? b->avg : BAND_FLOOR);
}

/* base: the glow in silence, min_brightness or idle_brightness */
static int brightness(int base, int max, float env)
{
    /* gamma ~2 so equal steps in loudness look like equal steps in light */
    return (int)(base + (max - base) * env * env + 0.5f);
}

/* SPI with DMA instead of RMT: C5 RMT has no DMA and Wi-Fi interrupts would corrupt the WS2812 stream */
static void strip_create(uint16_t count)
{
    if (strip) {
        led_strip_clear(strip); /* switch off LEDs beyond a shorter new length */
        led_strip_del(strip);
        strip = NULL;
    }
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_spi_config_t spi_cfg = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_cfg, &spi_cfg, &strip));
    strip_leds = count;
    ESP_LOGI(TAG, "%d WS2812 on GPIO%d", count, LED_GPIO);
}

static void led_task(void *arg)
{
    led_settings_t cfg;
    light_settings_t light = { 0 };
    dynamics_t dyn = { 0 };
    follower_t total = { 0 };
    band_follower_t bands[3] = { 0 };
    float glow = 0;  /* smoothed base brightness, follows idle on / off */
    const float glow_k = 1.0f - expf(-(float)FRAME_MS / IDLE_FADE_MS);
    uint32_t version = UINT32_MAX;
    uint32_t shown = UINT32_MAX;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        uint32_t v = settings_led_version();
        if (v != version) {
            version = v;
            settings_get_led(&cfg);
            light = cfg.light[settings_get_mode()];
            if (cfg.count != strip_leds) {
                strip_create(cfg.count);
            }
            dyn.attack = 1.0f - expf(-(float)FRAME_MS / light.attack_ms);
            dyn.release = 1.0f - expf(-(float)FRAME_MS / light.release_ms);
            dyn.peak_decay = powf(0.5f, (float)FRAME_MS / light.gain_halflife_ms);
            dyn.avg_k = 1.0f - dyn.peak_decay;
            shown = UINT32_MAX;
        }

        glow += ((atomic_load(&idle) ? light.idle_brightness : light.min_brightness) - glow) * glow_k;
        int base = (int)(glow + 0.5f);

        int r = 0, g = 0, b = 0;
        int level = brightness(base, light.max_brightness,
                               follow(&total, (float)atomic_load(&levels[LEVEL_TOTAL]), &dyn));
        switch (light.color) {
        case LIGHT_RED:
            r = level;
            break;
        case LIGHT_GREEN:
            g = level;
            break;
        case LIGHT_BLUE:
            b = level;
            break;
        default: { /* LIGHT_RGB */
            /* Brightness follows the loudness as in the single color modes; the color comes
             * from which band stands out: it gets the full brightness, the others less.
             * Scaling each band by its own auto gain instead would light all three: white. */
            float w[3], w_max = 0;
            for (int i = 0; i < 3; i++) {
                float e = band_excitement(&bands[i], (float)atomic_load(&levels[LEVEL_LOW + i]), &dyn);
                w[i] = powf(e, BAND_CONTRAST);
                w_max = w[i] > w_max ? w[i] : w_max;
            }
            int ch[3];
            for (int i = 0; i < 3; i++) {
                float share = w_max > 0 ? w[i] / w_max : 1.0f;
                ch[i] = base + (int)((level - base) * share + 0.5f);
            }
            r = ch[0];
            g = ch[1];
            b = ch[2];
            break;
        }
        }

        uint32_t forced = atomic_load(&override);
        if (forced != LED_MUSIC_NO_OVERRIDE) {
            r = forced >> 16 & 0xFF;
            g = forced >> 8 & 0xFF;
            b = forced & 0xFF;
        }

        uint32_t color = r << 16 | g << 8 | b;
        if (color != shown) {
            for (int i = 0; i < strip_leds; i++) {
                led_strip_set_pixel(strip, i, r, g, b);
            }
            led_strip_refresh(strip);
            shown = color;
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

void led_music_start(void)
{
    xTaskCreate(led_task, "led_music", 4096, NULL, 4, NULL);
}
