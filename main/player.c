#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "airplay.h"
#include "dlna.h"
#include "led_music.h"
#include "player.h"
#include "radio.h"
#include "sensor.h"
#include "settings.h"
#include "spotify.h"
#include "storage.h"

/* MAX98357 wiring (XIAO ESP32-C5) */
#define I2S_LRC_GPIO    GPIO_NUM_10 /* D10 */
#define I2S_BCLK_GPIO   GPIO_NUM_9  /* D9 */
#define I2S_DIN_GPIO    GPIO_NUM_8  /* D8 */

#define READ_CHUNK      4096 /* compressed bytes fed to the decoder at once */
#define PCM_BUF_SIZE    4608 /* one MP3 frame: 1152 samples * 2 ch * 16 bit */
#define IDLE_POLL_MS    500  /* when there is nothing to play */
#define TRIGGER_POLL_MS 100
#define RADIO_PREBUFFER (32 * 1024) /* ~2 s of 128 kbit/s before starting or after a dropout */
#define RADIO_RETRY_MS  2000
#define BASS_HZ         250  /* light music bands: bass below, treble above TREBLE_HZ */
#define TREBLE_HZ       2500

static const char *TAG = "player";

/* Start/stop buttons: when not running the star is silent and ignores the sensor */
static atomic_bool running = true;
static atomic_bool start_now; /* play a cycle right away, without waiting for the sensor */

static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static player_status_t status = PLAYER_IDLE;
static char status_track[TRACK_NAME_SIZE];

static void set_status(player_status_t s, const char *track)
{
    portENTER_CRITICAL(&status_lock);
    status = s;
    strlcpy(status_track, track ? track : "", sizeof(status_track));
    portEXIT_CRITICAL(&status_lock);
    led_music_set_idle(s == PLAYER_PAUSE || s == PLAYER_WAITING || s == PLAYER_STOPPED);
}

void player_get_status(player_status_t *s, char track[TRACK_NAME_SIZE])
{
    portENTER_CRITICAL(&status_lock);
    *s = status;
    strlcpy(track, status_track, TRACK_NAME_SIZE);
    portEXIT_CRITICAL(&status_lock);
}

bool player_is_playing(const char *name)
{
    portENTER_CRITICAL(&status_lock);
    bool playing = status == PLAYER_PLAYING && strcmp(status_track, name) == 0;
    portEXIT_CRITICAL(&status_lock);
    return playing;
}

static i2s_chan_handle_t tx_chan;
static uint32_t i2s_rate;
static int16_t *stereo_buf; /* mono input widened to stereo */
static size_t stereo_size;

/* The channel is created once, always stereo: mono is widened in software. Recreating it per
 * format would reallocate the DMA buffers and fragment the internal RAM. A new sample rate
 * only reconfigures the clock. */
static void i2s_start(uint32_t sample_rate)
{
    if (tx_chan && sample_rate == i2s_rate) {
        return;
    }
    if (tx_chan) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        ESP_ERROR_CHECK(i2s_channel_disable(tx_chan));
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
        i2s_rate = sample_rate;
        return;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* send silence instead of stale data on underrun */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    i2s_rate = sample_rate;
}

/* Splits the signal into bass / mids / treble for the RGB light music with two one-pole
 * low-pass filters in fixed point (Q15 coefficients): no FPU work per sample */
typedef struct {
    uint32_t rate;
    int32_t k_bass, k_treble;
    int32_t lp_bass, lp_treble;
} bands_t;

static bands_t bands;

static int32_t one_pole_k(uint32_t cutoff_hz, uint32_t rate)
{
    return (int32_t)((1.0f - expf(-2.0f * (float)M_PI * cutoff_hz / rate)) * 32768.0f);
}

static void play_pcm(int16_t *pcm, size_t size, int volume, int channels)
{
    if (bands.rate != i2s_rate) {
        bands.rate = i2s_rate;
        bands.k_bass = one_pole_k(BASS_HZ, i2s_rate);
        bands.k_treble = one_pole_k(TREBLE_HZ, i2s_rate);
    }

    size_t count = size / sizeof(int16_t);
    size_t frames = count / channels;
    uint64_t sq_total = 0, sq_low = 0, sq_mid = 0, sq_high = 0;
    for (size_t f = 0; f < frames; f++) {
        int16_t *s = pcm + f * channels;
        int32_t x = channels == 2 ? (s[0] + s[1]) / 2 : s[0];
        bands.lp_bass += (int32_t)(((int64_t)(x - bands.lp_bass) * bands.k_bass) >> 15);
        bands.lp_treble += (int32_t)(((int64_t)(x - bands.lp_treble) * bands.k_treble) >> 15);
        int32_t low = bands.lp_bass;
        int32_t mid = bands.lp_treble - bands.lp_bass;
        int32_t high = x - bands.lp_treble;
        sq_total += (int64_t)x * x;
        sq_low += (int64_t)low * low;
        sq_mid += (int64_t)mid * mid;
        sq_high += (int64_t)high * high;
    }
    if (frames) {
        led_music_set_levels(sqrtf((float)(sq_total / frames)), sqrtf((float)(sq_low / frames)),
                             sqrtf((float)(sq_mid / frames)), sqrtf((float)(sq_high / frames)));
    }

    for (size_t i = 0; i < count; i++) {
        int32_t v = (int32_t)pcm[i] * volume / 100;
        pcm[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
    }

    /* MAX98357 with SD floating plays (L+R)/2: mono goes to both slots */
    if (channels == 1) {
        if (stereo_size < size * 2) {
            int16_t *bigger = heap_caps_realloc(stereo_buf, size * 2, MALLOC_CAP_SPIRAM);
            if (bigger == NULL) {
                return;
            }
            stereo_buf = bigger;
            stereo_size = size * 2;
        }
        for (size_t i = 0; i < count; i++) {
            stereo_buf[2 * i] = stereo_buf[2 * i + 1] = pcm[i];
        }
        pcm = stereo_buf;
        size *= 2;
    }

    size_t written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_chan, pcm, size, &written, portMAX_DELAY));
}

/* Compressed audio coming from a file or from the radio buffer */
typedef struct audio_source {
    esp_audio_simple_dec_type_t type;
    /* Up to len bytes; 0 means end of stream */
    size_t (*read)(struct audio_source *src, uint8_t *buf, size_t len);
    /* Volume percent for the next frame, or -1 to stop now */
    int (*volume)(struct audio_source *src);
    /* Progress, kept by decode() */
    uint64_t frames_played;
    uint32_t sample_rate;
} audio_source_t;

/* Decode a source to the speaker; returns false if nothing could be decoded */
static bool decode(audio_source_t *src)
{
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = src->type,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    uint8_t *in = malloc(READ_CHUNK);
    uint32_t out_size = PCM_BUF_SIZE;
    uint8_t *out = malloc(out_size);
    bool decoded = false;
    if (in == NULL || out == NULL || esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Cannot start the decoder");
        goto done;
    }

    esp_audio_simple_dec_raw_t raw = { .buffer = in };
    bool need_more = true;

    for (;;) {
        int volume = src->volume(src);
        if (volume < 0) {
            break;
        }

        /* Keep the input buffer topped up: move the unconsumed tail to the front, read more */
        if ((need_more || raw.len < READ_CHUNK / 2) && !raw.eos) {
            memmove(in, raw.buffer, raw.len);
            raw.buffer = in;
            size_t got = src->read(src, in + raw.len, READ_CHUNK - raw.len);
            raw.len += got;
            raw.eos = got == 0;
            need_more = false;
        }
        if (raw.len == 0) {
            break;
        }

        esp_audio_simple_dec_out_t frame = {
            .buffer = out,
            .len = out_size,
        };
        esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *bigger = realloc(out, frame.needed_size);
            if (bigger == NULL) {
                ESP_LOGE(TAG, "No memory for %" PRIu32 " byte PCM buffer", frame.needed_size);
                break;
            }
            out = bigger;
            out_size = frame.needed_size;
            continue;
        }
        if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (raw.eos) {
                break;
            }
            need_more = true;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Decode error %d", ret);
            break;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;

        if (frame.decoded_size == 0) {
            if (raw.consumed == 0) {
                if (raw.eos) {
                    break; /* undecodable tail */
                }
                need_more = true;
            }
            continue;
        }
        esp_audio_simple_dec_info_t info;
        esp_audio_simple_dec_get_info(dec, &info);
        i2s_start(info.sample_rate);
        int channels = info.channel == 2 ? 2 : 1;
        play_pcm((int16_t *)frame.buffer, frame.decoded_size, volume, channels);
        src->frames_played += frame.decoded_size / (sizeof(int16_t) * channels);
        src->sample_rate = info.sample_rate;
        decoded = true;
    }

done:
    if (dec) {
        esp_audio_simple_dec_close(dec);
    }
    free(in);
    free(out);
    led_music_set_levels(0, 0, 0, 0);
    return decoded;
}

static esp_audio_simple_dec_type_t decoder_type(radio_codec_t codec)
{
    switch (codec) {
    case RADIO_CODEC_AAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case RADIO_CODEC_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case RADIO_CODEC_WAV: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case RADIO_CODEC_M4A: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    default: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
}

/* A phone casting (DLNA / AirPlay / Spotify) takes over from the playlist, the radio and the sensor */
static bool cast_active(void)
{
    return airplay_active() || spotify_active() || dlna_session_open();
}

/* ---- Playlist: MP3 files from storage ---- */

typedef struct {
    audio_source_t base;
    FILE *file;
    const char *name;
} file_source_t;

static size_t file_read(audio_source_t *src, uint8_t *buf, size_t len)
{
    return fread(buf, 1, len, ((file_source_t *)src)->file);
}

/* Re-read every frame, so web UI changes apply at once; a disabled or deleted track,
 * or a switch to the radio mode, stops it right away */
static int file_volume(audio_source_t *src)
{
    track_settings_t ts;
    if (!running || cast_active() || settings_get_mode() != PLAYER_MODE_PLAYLIST ||
        !settings_find_track(((file_source_t *)src)->name, &ts) || !ts.enabled) {
        return -1;
    }
    return ts.volume;
}

static void play_track(const char *name)
{
    char path[128];
    storage_path(name, path, sizeof(path));
    file_source_t src = {
        .base = { .type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, .read = file_read, .volume = file_volume },
        .file = fopen(path, "rb"),
        .name = name,
    };
    if (src.file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return;
    }
    decode(&src.base);
    fclose(src.file);
}

static bool playlist_keep_playing(void)
{
    return running && !cast_active() && settings_get_mode() == PLAYER_MODE_PLAYLIST;
}

/* vTaskDelay that ends early when keep_playing() turns false */
static void pause_while(uint32_t ms, bool (*keep_playing)(void))
{
    for (uint32_t waited = 0; waited < ms && keep_playing(); waited += TRIGGER_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(ms - waited < TRIGGER_POLL_MS ? ms - waited : TRIGGER_POLL_MS));
    }
}

/* One pass over the enabled tracks; false if none is enabled */
static bool play_cycle(void)
{
    bool played = false;
    char name[TRACK_NAME_SIZE];
    track_settings_t ts;
    /* By index: tracks may be uploaded or deleted meanwhile, a skipped or repeated one is fine */
    for (size_t i = 0; settings_get_track(i, name, &ts); i++) {
        if (!playlist_keep_playing()) {
            break;
        }
        if (!ts.enabled) {
            continue;
        }
        played = true;
        ESP_LOGI(TAG, "Playing %s x%d, volume %d%%, pause %" PRIu32 " ms",
                 name, ts.repeat, ts.volume, ts.pause_ms);
        set_status(PLAYER_PLAYING, name);
        /* Re-read after each repeat: settings may change while playing */
        for (int r = 0; r < ts.repeat && ts.enabled; r++) {
            play_track(name);
            if (!settings_find_track(name, &ts) || !playlist_keep_playing()) {
                break;
            }
        }
        if (playlist_keep_playing()) {
            set_status(PLAYER_PAUSE, name);
            pause_while(ts.pause_ms, playlist_keep_playing);
        }
    }
    return played;
}

/* ---- Radio: stream from the radio buffer ---- */

typedef struct {
    audio_source_t base;
    char url[RADIO_URL_SIZE];
    uint32_t generation;
    TickType_t deadline; /* 0: play until the mode or station changes */
    bool buffering;
} radio_source_t;

static bool radio_mode_on(void)
{
    return running && !cast_active() && settings_get_mode() == PLAYER_MODE_RADIO;
}

static bool radio_keep_playing(radio_source_t *rs)
{
    if (!radio_mode_on() || radio_generation() != rs->generation) {
        return false;
    }
    if (rs->deadline && (int32_t)(xTaskGetTickCount() - rs->deadline) >= 0) {
        return false;
    }
    radio_station_t station;
    return settings_get_current_station(&station) && strcmp(station.url, rs->url) == 0;
}

/* Waits through network dropouts: after the buffer runs dry, refills it before resuming */
static size_t radio_source_read(audio_source_t *src, uint8_t *buf, size_t len)
{
    radio_source_t *rs = (radio_source_t *)src;
    for (;;) {
        if (!radio_keep_playing(rs)) {
            return 0;
        }
        radio_status_t st;
        radio_get_status(&st);
        if (st.buffered >= (rs->buffering ? RADIO_PREBUFFER : 1)) {
            rs->buffering = false;
            return radio_read(buf, len);
        }
        if (!rs->buffering) {
            ESP_LOGW(TAG, "Radio buffer ran dry, refilling");
            rs->buffering = true;
            led_music_set_levels(0, 0, 0, 0); /* silence: let the light fade */
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static int radio_source_volume(audio_source_t *src)
{
    return radio_keep_playing((radio_source_t *)src) ? settings_get_radio_volume() : -1;
}

/* Play the selected station until the mode changes or the deadline passes (0: no deadline).
 * Station changes and broken streams are handled here by reconnecting. */
static void play_radio(TickType_t deadline)
{
    while (radio_mode_on() && !(deadline && (int32_t)(xTaskGetTickCount() - deadline) >= 0)) {
        radio_station_t station;
        if (!settings_get_current_station(&station)) {
            radio_stop();
            set_status(PLAYER_IDLE, NULL);
            pause_while(IDLE_POLL_MS, radio_mode_on);
            continue;
        }

        static radio_source_t rs; /* holds a URL, too big for the stack */
        rs = (radio_source_t){
            .base = { .read = radio_source_read, .volume = radio_source_volume },
            .deadline = deadline,
            .buffering = true,
        };
        strlcpy(rs.url, station.url, sizeof(rs.url));
        rs.generation = radio_play(rs.url, NULL, true, 0);
        set_status(PLAYER_RADIO, station.name);
        ESP_LOGI(TAG, "Radio: %s", station.name);

        /* The codec is known once the stream is connected */
        radio_status_t st;
        for (;;) {
            radio_get_status(&st);
            if (!radio_keep_playing(&rs) || (st.generation == rs.generation && st.state == RADIO_STREAMING)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TRIGGER_POLL_MS));
        }
        if (!radio_keep_playing(&rs)) {
            continue;
        }
        rs.base.type = decoder_type(st.codec);
        if (!decode(&rs.base) && radio_keep_playing(&rs)) {
            ESP_LOGW(TAG, "%s: stream could not be decoded", station.name);
        }
        if (radio_keep_playing(&rs)) {
            pause_while(RADIO_RETRY_MS, radio_mode_on); /* stream ended or broke: reconnect */
        }
    }
    radio_stop();
}

/* ---- Casting ---- */

/* Phone volume 0..100 to our gain in percent: squared, so the phone's slider feels even */
static int cast_gain(uint8_t volume)
{
    return volume * volume / 100;
}

typedef struct {
    audio_source_t base;
    dlna_job_t job;
    radio_codec_t codec;
    uint32_t bytes_read;    /* to estimate the bitrate when the length is unknown */
    int64_t last_info_us;
} dlna_source_t;

/* The job is still the one being played; paused (may be NULL) tells if it is on hold */
static bool dlna_current(const dlna_source_t *ds, bool *paused)
{
    static dlna_job_t now; /* holds a long URL; only the player task calls this */
    if (airplay_active() || spotify_active() || !dlna_get_job(&now) || now.generation != ds->job.generation) {
        return false;
    }
    if (paused) {
        *paused = now.paused;
    }
    return true;
}

/* Holds while paused; 0 at the end of the file or when the phone moves on */
static size_t dlna_source_read(audio_source_t *src, uint8_t *buf, size_t len)
{
    dlna_source_t *ds = (dlna_source_t *)src;
    for (;;) {
        bool paused;
        if (!dlna_current(ds, &paused)) {
            return 0;
        }
        radio_status_t st;
        radio_get_status(&st);
        if (!paused && st.buffered > 0) {
            size_t n = radio_read(buf, len);
            ds->bytes_read += n;
            return n;
        }
        if (st.buffered == 0 && (st.state == RADIO_ENDED || st.state == RADIO_ERROR)) {
            return 0;
        }
        led_music_set_levels(0, 0, 0, 0); /* paused or waiting for data: let the light fade */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static int dlna_source_volume(audio_source_t *src)
{
    dlna_source_t *ds = (dlna_source_t *)src;
    if (!dlna_current(ds, NULL)) {
        return -1;
    }
    if (src->sample_rate) {
        uint32_t played_ms = src->frames_played * 1000 / src->sample_rate;
        dlna_set_position(ds->job.generation, ds->job.start_ms + played_ms);

        /* Once a second: what the phone needs to offer seeking */
        int64_t now = esp_timer_get_time();
        if (now - ds->last_info_us > 1000000 && played_ms > 1000) {
            ds->last_info_us = now;
            radio_status_t st;
            radio_get_status(&st);
            /* Raw MP3 / ADTS AAC frames resync anywhere; FLAC, WAV, M4A need their header */
            bool format_ok = ds->codec == RADIO_CODEC_MP3 || ds->codec == RADIO_CODEC_AAC;
            uint32_t bps = (uint64_t)ds->bytes_read * 8000 / played_ms;
            dlna_set_stream_info(ds->job.generation, st.content_length, bps, st.seekable && format_ok);
        }
    }
    return dlna_muted() ? 0 : cast_gain(dlna_volume());
}

static void play_dlna(void)
{
    static dlna_source_t ds;
    if (!dlna_get_job(&ds.job)) {
        return;
    }
    ds.base = (audio_source_t){ .read = dlna_source_read, .volume = dlna_source_volume };
    ds.bytes_read = 0;
    ds.last_info_us = 0;
    uint32_t gen = radio_play(ds.job.url, ds.job.mime, false, ds.job.byte_offset);
    set_status(PLAYER_CAST, ds.job.title[0] ? ds.job.title : "DLNA");
    ESP_LOGI(TAG, "DLNA: %s", ds.job.title[0] ? ds.job.title : ds.job.url);

    /* The format is known once connected */
    radio_status_t st;
    for (;;) {
        if (!dlna_current(&ds, NULL)) {
            radio_stop();
            return;
        }
        radio_get_status(&st);
        if (st.generation == gen && st.state != RADIO_CONNECTING && st.state != RADIO_STOPPED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); /* a file on the local network connects in milliseconds */
    }
    bool failed = st.state == RADIO_ERROR && st.buffered == 0;
    if (!failed) {
        ds.codec = st.codec;
        ds.base.type = decoder_type(st.codec);
        failed = !decode(&ds.base);
    }
    radio_stop();
    dlna_finished(ds.job.generation, failed); /* ignored if the phone moved on meanwhile */
}

static void play_airplay(void)
{
    static int16_t pcm[352 * 2]; /* one AirPlay packet: 352 stereo frames */
    set_status(PLAYER_CAST, "AirPlay");
    ESP_LOGI(TAG, "AirPlay");
    i2s_start(44100);
    while (airplay_active()) {
        size_t frames = airplay_read(pcm, 352, 100);
        if (frames) {
            play_pcm(pcm, frames * 2 * sizeof(int16_t), cast_gain(airplay_volume()), 2);
        } else {
            led_music_set_levels(0, 0, 0, 0); /* paused on the phone: let the light fade */
        }
    }
    led_music_set_levels(0, 0, 0, 0);
}

/* Cut a string snprintf truncated back to whole UTF-8 characters */
static void utf8_trim(char *s)
{
    size_t n = strlen(s);
    size_t start = n;
    while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80) {
        start--; /* continuation bytes of the last character */
    }
    if (start > 0) {
        unsigned char lead = s[start - 1];
        size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (start - 1) < len) {
            s[start - 1] = '\0';
        }
    }
}

static void play_spotify(void)
{
    static int16_t *pcm; /* 1024 stereo frames, in PSRAM: internal RAM is short */
    static spotify_status_t st;
    if (pcm == NULL) {
        pcm = heap_caps_malloc(1024 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (pcm == NULL) {
            return;
        }
    }
    char shown[TRACK_NAME_SIZE] = "";
    int64_t next_info = 0;
    ESP_LOGI(TAG, "Spotify");
    i2s_start(44100);
    /* AirPlay wins when a phone starts it meanwhile */
    while (spotify_active() && !airplay_active()) {
        int64_t now = esp_timer_get_time();
        if (now >= next_info) {
            next_info = now + 500000;
            spotify_get_status(&st);
            char track[TRACK_NAME_SIZE];
            snprintf(track, sizeof(track), st.title[0] ? "Spotify: %s" : "Spotify", st.title);
            utf8_trim(track);
            if (strcmp(track, shown) != 0) {
                strlcpy(shown, track, sizeof(shown));
                set_status(PLAYER_CAST, shown);
            }
        }
        size_t frames = spotify_read(pcm, 1024, 100);
        if (frames) {
            play_pcm(pcm, frames * 2 * sizeof(int16_t), cast_gain(spotify_volume()), 2);
        } else {
            led_music_set_levels(0, 0, 0, 0); /* paused in the app or buffering: let the light fade */
        }
    }
    led_music_set_levels(0, 0, 0, 0);
}

/* Play whatever phones cast until they stop; true if anything was cast */
static bool play_cast(void)
{
    bool played = false;
    while (cast_active()) {
        played = true;
        if (airplay_active()) {
            play_airplay();
        } else if (spotify_active()) {
            play_spotify();
        } else if (dlna_get_job(NULL)) {
            play_dlna();
        } else {
            /* Between tracks: the phone sends the next one in a moment */
            led_music_set_levels(0, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return played;
}

/* ---- Main loop ---- */

/* Block while stopped. With the sensor enabled, also until it sees a target and the
 * cooldown since the last cycle is over, unless "start" was pressed. */
static void wait_for_trigger(bool first_cycle, TickType_t cycle_end)
{
    bool waiting = false;
    for (;;) {
        if (cast_active()) {
            return;
        }
        if (!running) {
            if (status != PLAYER_STOPPED) {
                ESP_LOGI(TAG, "Stopped");
                set_status(PLAYER_STOPPED, NULL);
            }
            waiting = false;
            vTaskDelay(pdMS_TO_TICKS(TRIGGER_POLL_MS));
            continue;
        }
        if (atomic_exchange(&start_now, false)) {
            ESP_LOGI(TAG, "Started from the web page");
            return;
        }
        sensor_settings_t ss;
        settings_get_sensor(&ss);
        if (!ss.enabled) {
            return;
        }
        uint64_t since_ms = (uint64_t)(xTaskGetTickCount() - cycle_end) * portTICK_PERIOD_MS;
        bool cooled = first_cycle || since_ms >= ss.cooldown_s * 1000ULL;
        if (cooled && sensor_triggered(ss.trigger)) {
            sensor_state_t s;
            sensor_get_state(&s);
            ESP_LOGI(TAG, "Sensor trigger: target %d at %d cm", s.target, s.distance_cm);
            return;
        }
        if (!waiting) {
            ESP_LOGI(TAG, "Waiting for the sensor");
            set_status(PLAYER_WAITING, NULL);
            waiting = true;
        }
        vTaskDelay(pdMS_TO_TICKS(TRIGGER_POLL_MS));
    }
}

static void player_task(void *arg)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    bool first_cycle = true;
    TickType_t cycle_end = 0;

    for (;;) {
        if (play_cast()) {
            /* Back to the own mode; with the sensor it waits for it again */
            cycle_end = xTaskGetTickCount();
            first_cycle = false;
            continue;
        }
        wait_for_trigger(first_cycle, cycle_end);
        first_cycle = false;
        if (cast_active()) {
            continue;
        }

        bool played = true;
        if (settings_get_mode() == PLAYER_MODE_RADIO) {
            /* With the sensor, a "cycle" of the radio is a fixed time */
            sensor_settings_t ss;
            settings_get_sensor(&ss);
            TickType_t deadline = 0;
            if (ss.enabled) {
                deadline = xTaskGetTickCount() + pdMS_TO_TICKS(settings_get_radio_minutes() * 60000ULL);
                deadline += deadline == 0; /* 0 means no deadline */
            }
            play_radio(deadline);
        } else {
            played = play_cycle();
        }
        cycle_end = xTaskGetTickCount();
        if (!played && running) {
            set_status(PLAYER_IDLE, NULL);
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
        }
    }
}

void player_play(void)
{
    /* While a cycle is on, a pending start would add one more right after it */
    player_status_t s = status;
    if (!running || s == PLAYER_STOPPED || s == PLAYER_WAITING || s == PLAYER_IDLE) {
        atomic_store(&start_now, true);
    }
    atomic_store(&running, true);
}

void player_stop(void)
{
    atomic_store(&running, false);
    dlna_stop();
    airplay_stop();
    spotify_stop();
}

void player_start(void)
{
    radio_init();
    /* MP3/AAC decoder needs more stack than the default main task has */
    xTaskCreate(player_task, "player", 8192, NULL, 5, NULL);
}
