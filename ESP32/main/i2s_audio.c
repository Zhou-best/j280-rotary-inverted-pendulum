#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "i2s_audio.h"
#include "tft_spi.h"
#include "tft_spi.h"

static const char *TAG = "I2S_AUDIO";


#define I2S_RX_DMA_DESC_NUM 12

static i2s_chan_handle_t s_rx_chan = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static SemaphoreHandle_t s_audio_lock = NULL;
volatile bool s_i2s_tx_active = false;
volatile bool s_i2s_rx_grabbed = false;


static void i2s_do_stop_rx(void);


esp_err_t i2s_audio_init(void)
{
    if (!s_audio_lock) s_audio_lock = xSemaphoreCreateMutex();
    if (!s_audio_lock) return ESP_ERR_NO_MEM;
    if (s_rx_chan) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = I2S_RX_DMA_DESC_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_AUDIO_SAMPLE_RATE),

        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = I2S_AUDIO_BCLK, .ws = I2S_AUDIO_LRCK,
            .dout = I2S_GPIO_UNUSED, .din = I2S_AUDIO_DIN,
            .invert_flags = { .bclk_inv = false, .ws_inv = false },
        },
    };

    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    ESP_LOGI(TAG, "I2S RX-Only: 16kHz MONO LEFT (INMP441)");
    return ESP_OK;
}

esp_err_t i2s_audio_deinit(void)
{
    i2s_do_stop_rx();
    if (s_tx_chan) { i2s_channel_disable(s_tx_chan); i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    ESP_LOGI(TAG, "I2S deinit complete");
    return ESP_OK;
}

static void i2s_do_stop_rx(void)
{
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        vTaskDelay(pdMS_TO_TICKS(10));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
}

i2s_chan_handle_t i2s_audio_get_rx_channel(void) { return s_rx_chan; }

void i2s_audio_grab_rx(void) {
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    s_i2s_rx_grabbed = true;


    vTaskDelay(pdMS_TO_TICKS(220));
}
void i2s_audio_release_rx(void) {
    s_i2s_rx_grabbed = false;
    xSemaphoreGive(s_audio_lock);
}


size_t i2s_audio_read_chunk(int16_t *buf, size_t max_samples, uint32_t timeout_ms)
{
    if (!s_rx_chan) return 0;
    size_t chunk16 = max_samples < 256 ? max_samples : 256;
    int16_t raw[256];
    size_t collected = 0;
    int failures = 0;
    while (collected < max_samples) {
        size_t to_read = (max_samples - collected) < chunk16 ? (max_samples - collected) : chunk16;
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_chan, raw, to_read * sizeof(int16_t), &bytes_read, timeout_ms);
        if (ret != ESP_OK) { if (++failures >= 5) break; vTaskDelay(1); continue; }
        if (bytes_read == 0) { if (++failures >= 10) break; continue; }
        failures = 0;
        memcpy(&buf[collected], raw, bytes_read);
        collected += bytes_read / sizeof(int16_t);
    }
    return collected;
}

esp_err_t i2s_audio_write(const int16_t *data, size_t samples, uint32_t timeout_ms)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    size_t written = 0;
    return i2s_channel_write(s_tx_chan, data, samples * sizeof(int16_t),
                              &written, timeout_ms);
}

esp_err_t i2s_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;
    size_t read = 0;
    return i2s_channel_read(s_rx_chan, buf, samples * sizeof(int16_t), &read, timeout_ms);
}

int16_t *i2s_audio_record_ms(uint32_t duration_ms, size_t *out_samples)
{
    size_t total = (size_t)((uint64_t)I2S_AUDIO_SAMPLE_RATE * duration_ms / 1000);
    int16_t *buf = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { *out_samples = 0; return NULL; }
    size_t collected = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (collected < total && esp_timer_get_time() < deadline) {
        size_t n = i2s_audio_read_chunk(&buf[collected], total - collected, 200);
        collected += n;
    }
    *out_samples = collected;
    return buf;
}


static esp_err_t i2s_audio_play_impl(const int16_t *const *data, const size_t *samples,
                                     size_t count, uint32_t rate)
{
    if (!data || !samples || count == 0) return ESP_ERR_INVALID_ARG;
    if (!s_audio_lock) return ESP_ERR_INVALID_STATE;
    if (rate == 0) rate = I2S_AUDIO_SAMPLE_RATE;
    size_t total_samples = 0;
    for (size_t c = 0; c < count; c++) total_samples += samples[c];
    if (total_samples == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_audio_lock, portMAX_DELAY);

    ESP_LOGI(TAG, "TX: begin %u clips, %u samples (%.2fs @%uHz)",
             (unsigned)count, (unsigned)total_samples,
             (double)total_samples / rate, (unsigned)rate);

    s_i2s_tx_active = true;


    vTaskDelay(pdMS_TO_TICKS(120));


    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        vTaskDelay(pdMS_TO_TICKS(10));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }


    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX: i2s_new_channel failed: %s", esp_err_to_name(ret));
        goto restore_rx;
    }

    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),


        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_AUDIO_BCLK, .ws = I2S_AUDIO_LRCK,
            .dout = I2S_AUDIO_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_chan, &tx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX: init_std_mode failed: %s", esp_err_to_name(ret));
        goto del_tx;
    }
    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX: channel_enable failed: %s", esp_err_to_name(ret));
        goto del_tx;
    }
    ESP_LOGI(TAG, "TX: channel up (16kHz 16bit STEREO, dout=GPIO%d)", (int)I2S_AUDIO_DOUT);


    vTaskDelay(pdMS_TO_TICKS(200));


    #define TX_CHUNK_MONO  512
    #define FADE_SAMPLES   240
    {
        int16_t stereo_buf[TX_CHUNK_MONO * 2];
        size_t written = 0;
        for (size_t c = 0; c < count && ret == ESP_OK; c++) {
            const int16_t *src = data[c];
            size_t total = samples[c];
            size_t pos = 0;
            while (pos < total) {
                size_t n = (total - pos) < TX_CHUNK_MONO ? (total - pos) : TX_CHUNK_MONO;
                for (size_t i = 0; i < n; i++) {
                    int16_t v = src[pos + i];
                    if (pos + i < FADE_SAMPLES)
                        v = (int16_t)((int32_t)v * (pos + i) / FADE_SAMPLES);
                    stereo_buf[i * 2]     = v;
                    stereo_buf[i * 2 + 1] = v;
                }
                size_t w = 0;


                ret = i2s_channel_write(s_tx_chan, stereo_buf,
                                         n * 2 * sizeof(int16_t), &w,
                                         1000);
                if (ret != ESP_OK) {


                    size_t advanced = w / (2 * sizeof(int16_t));
                    if (advanced > 0) {
                        pos += advanced;
                        written += advanced;
                        ESP_LOGW(TAG, "TX write partial clip %u @%u/%u (%u bytes), retry: %s",
                                 (unsigned)c, (unsigned)pos, (unsigned)total,
                                 (unsigned)w, esp_err_to_name(ret));
                        ret = ESP_OK;
                        continue;
                    }
                    ESP_LOGW(TAG, "TX write err clip %u @%u/%u: %s",
                             (unsigned)c, (unsigned)pos, (unsigned)total, esp_err_to_name(ret));
                    break;
                }
                pos += n;
                written += n;
            }
        }
        ESP_LOGI(TAG, "TX: wrote %u/%u mono samples (%s)",
                 (unsigned)written, (unsigned)total_samples,
                 (written >= total_samples && ret == ESP_OK) ? "OK" : "INCOMPLETE");
    }


    uint32_t drain_ms = (uint32_t)((uint64_t)total_samples * 1000 / rate) / 8 + 200;
    if (drain_ms < 200) drain_ms = 200;
    vTaskDelay(pdMS_TO_TICKS(drain_ms));

del_tx:
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        vTaskDelay(pdMS_TO_TICKS(10));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }

restore_rx:

    if (!s_rx_chan) {
        i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        rx_chan_cfg.auto_clear = true;
        rx_chan_cfg.dma_desc_num = I2S_RX_DMA_DESC_NUM;
        if (i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan) == ESP_OK) {
            i2s_std_config_t rx_cfg = {
                .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_AUDIO_SAMPLE_RATE),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                    .bclk = I2S_AUDIO_BCLK, .ws = I2S_AUDIO_LRCK,
                    .dout = I2S_GPIO_UNUSED, .din = I2S_AUDIO_DIN,
                    .invert_flags = { .bclk_inv = false, .ws_inv = false },
                },
            };
            rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
            i2s_channel_init_std_mode(s_rx_chan, &rx_cfg);
            i2s_channel_enable(s_rx_chan);
            ESP_LOGI(TAG, "I2S RX resumed (MONO LEFT)");
        } else {
            ESP_LOGE(TAG, "RX restore failed!");
        }
    }


    vTaskDelay(pdMS_TO_TICKS(80));
    s_i2s_tx_active = false;
    xSemaphoreGive(s_audio_lock);


    tft_request_redraw();
    return ret;
}

esp_err_t i2s_audio_play(const int16_t *data, size_t samples)
{
    return i2s_audio_play_impl(&data, &samples, 1, I2S_AUDIO_SAMPLE_RATE);
}

esp_err_t i2s_audio_play_rate(const int16_t *data, size_t samples, uint32_t sample_rate)
{
    return i2s_audio_play_impl(&data, &samples, 1, sample_rate);
}

esp_err_t i2s_audio_play_multi(const int16_t *const *data, const size_t *samples,
                               size_t count, uint32_t sample_rate)
{
    return i2s_audio_play_impl(data, samples, count, sample_rate);
}


void i2s_test_tone(void)
{
    size_t total = I2S_AUDIO_SAMPLE_RATE * 2;
    int16_t *buf = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return;
    for (size_t i = 0; i < total; i++)
        buf[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * i / I2S_AUDIO_SAMPLE_RATE));
    ESP_LOGI(TAG, "=== Test tone 440Hz 2s ===");
    i2s_audio_play(buf, total);
    ESP_LOGI(TAG, "=== Test tone done ===");
    free(buf);
}


void i2s_speaker_count_test(void)
{
    const int beep_ms = 250, gap_ms = 200, between_ms = 600;
    const int freq = 600;
    size_t beep_samples = I2S_AUDIO_SAMPLE_RATE * beep_ms / 1000;
    size_t gap_samples  = I2S_AUDIO_SAMPLE_RATE * gap_ms / 1000;
    size_t between_samples = I2S_AUDIO_SAMPLE_RATE * between_ms / 1000;
    size_t total_samples = (beep_samples + gap_samples) * 6 + between_samples * 2 + gap_samples;

    int16_t *buf = heap_caps_malloc(total_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Speaker test: OOM");
        return;
    }
    memset(buf, 0, total_samples * sizeof(int16_t));

    size_t pos = 0;
    for (int count = 1; count <= 3; count++) {
        for (int b = 0; b < count; b++) {
            for (size_t i = 0; i < beep_samples && pos < total_samples; i++) {
                buf[pos++] = (int16_t)(10000.0 * sin(2.0 * M_PI * freq * i / I2S_AUDIO_SAMPLE_RATE));
            }
            pos += gap_samples;
        }
        pos += between_samples;
    }

    ESP_LOGI(TAG, "=== Speaker count test: 1-2-3 beeps (%.1fs) ===",
             (double)total_samples / I2S_AUDIO_SAMPLE_RATE);
    esp_err_t st_ret = i2s_audio_play(buf, total_samples);


    ESP_LOGI(TAG, "=== Speaker test done (ret=%s). "
                  "TX logs above OK but silent => hardware: "
                  "MAX98357A SD pin must be HIGH, check DIN->GPIO%d, speaker, supply ===",
             esp_err_to_name(st_ret), (int)I2S_AUDIO_DOUT);
    free(buf);
}

void i2s_audio_start_clock_keeper(void)
{
    ESP_LOGI(TAG, "ck: not needed");
}
