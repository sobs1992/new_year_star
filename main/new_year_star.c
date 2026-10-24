#include <math.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

/* MAX98357 wiring (XIAO ESP32-C5) */
#define I2S_LRC_GPIO    GPIO_NUM_10 /* D10 */
#define I2S_BCLK_GPIO   GPIO_NUM_9  /* D9 */
#define I2S_DIN_GPIO    GPIO_NUM_8  /* D8 */

#define SAMPLE_RATE     48000
#define TONE_HZ         1000
#define TONE_MS         500
#define CYCLE_MS        1000
#define AMPLITUDE       12000                   /* ~37% of full scale */
#define FADE_SAMPLES    (SAMPLE_RATE / 100)     /* 10 ms fade in/out against clicks */

#define WAVE_SAMPLES    (SAMPLE_RATE / TONE_HZ) /* one sine period */
#define TONE_SAMPLES    (SAMPLE_RATE * TONE_MS / 1000)
#define CYCLE_SAMPLES   (SAMPLE_RATE * CYCLE_MS / 1000)
#define CHUNK_SAMPLES   320

static const char *TAG = "tone";

static i2s_chan_handle_t tx_chan;
static int16_t wave[WAVE_SAMPLES];

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    /* Same mono sample in both slots: MAX98357 plays (L+R)/2 with SD floating */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

static int16_t tone_sample(int n)
{
    if (n >= TONE_SAMPLES) {
        return 0;
    }
    int32_t s = wave[n % WAVE_SAMPLES];
    if (n < FADE_SAMPLES) {
        s = s * n / FADE_SAMPLES;
    } else if (n >= TONE_SAMPLES - FADE_SAMPLES) {
        s = s * (TONE_SAMPLES - n) / FADE_SAMPLES;
    }
    return (int16_t)s;
}

void app_main(void)
{
    for (int i = 0; i < WAVE_SAMPLES; i++) {
        wave[i] = (int16_t)(AMPLITUDE * sinf(2.0f * (float)M_PI * i / WAVE_SAMPLES));
    }

    i2s_init();
    ESP_LOGI(TAG, "%d Hz tone, %d ms every %d ms", TONE_HZ, TONE_MS, CYCLE_MS);

    static int16_t buf[CHUNK_SAMPLES];
    for (;;) {
        /* i2s_channel_write blocks on DMA, so one cycle takes exactly CYCLE_MS */
        for (int pos = 0; pos < CYCLE_SAMPLES; pos += CHUNK_SAMPLES) {
            for (int i = 0; i < CHUNK_SAMPLES; i++) {
                buf[i] = tone_sample(pos + i);
            }
            size_t written;
            ESP_ERROR_CHECK(i2s_channel_write(tx_chan, buf, sizeof(buf), &written, portMAX_DELAY));
        }
    }
}
