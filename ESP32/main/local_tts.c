#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "i2s_audio.h"
#include "local_tts.h"

static const char *TAG = "LOCAL_TTS";

static esp_tts_handle_t s_tts = NULL;
static esp_tts_voice_t *s_voice = NULL;
static const void *s_voice_data = NULL;
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static QueueHandle_t s_tts_queue = NULL;

static volatile bool s_tts_busy = false;
static void tts_task(void *arg);


esp_err_t local_tts_init(void)
{
    if (s_tts_queue) return ESP_OK;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (!partition) {
        ESP_LOGE(TAG, "voice_data partition not found! "
                      "Flash the voice .dat file first.");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "voice_data partition: offset=0x%lx size=%lu KiB",
             (unsigned long)partition->address,
             (unsigned long)partition->size / 1024);


    esp_err_t ret = esp_partition_mmap(partition, 0, partition->size,
                                        ESP_PARTITION_MMAP_DATA,
                                        &s_voice_data, &s_mmap_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(ret));
        return ret;
    }


    s_voice = esp_tts_voice_set_init(&esp_tts_voice_template,
                                      (int16_t *)s_voice_data);
    if (!s_voice) {
        ESP_LOGE(TAG, "esp_tts_voice_set_init failed");
        esp_partition_munmap(s_mmap_handle);
        return ESP_FAIL;
    }


    s_tts = esp_tts_create(s_voice);
    if (!s_tts) {
        ESP_LOGE(TAG, "esp_tts_create failed");
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        esp_partition_munmap(s_mmap_handle);
        return ESP_FAIL;
    }

    s_tts_queue = xQueueCreate(4, sizeof(char *));

    if (!s_tts_queue || xTaskCreate(tts_task, "tts_play", 10240, NULL, 3, NULL) != pdPASS) {
        if (s_tts_queue) vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        esp_tts_destroy(s_tts);
        s_tts = NULL;
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        esp_partition_munmap(s_mmap_handle);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Local TTS (xiaole) initialized: %s, %dHz, %dbit",
             s_voice->voice_name ? s_voice->voice_name : "?",
             (int)s_voice->sample_rate, (int)s_voice->bit_width);
    return ESP_OK;
}


static esp_err_t local_tts_play_sync(const char *chinese_text)
{
    if (!s_tts) return ESP_ERR_INVALID_STATE;
    if (!chinese_text || strlen(chinese_text) == 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "TTS: \"%s\"", chinese_text);

    esp_tts_stream_reset(s_tts);

    if (!esp_tts_parse_chinese(s_tts, chinese_text)) {
        ESP_LOGE(TAG, "Parse failed: %s", chinese_text);
        return ESP_FAIL;
    }

    #define TTS_CHUNK_SIZE  4096
    size_t buf_cap = TTS_CHUNK_SIZE;
    int16_t *pcm_buf = heap_caps_malloc(buf_cap * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM);
    if (!pcm_buf) return ESP_ERR_NO_MEM;
    size_t total_samples = 0;

    int speed = 5;
    while (1) {
        int chunk_len = 0;
        short *chunk = esp_tts_stream_play(s_tts, &chunk_len, speed);
        if (chunk_len == 0 || !chunk) break;
        if (total_samples + chunk_len > buf_cap) {
            buf_cap = (total_samples + chunk_len) * 2;
            int16_t *new_buf = heap_caps_realloc(pcm_buf,
                                                  buf_cap * sizeof(int16_t),
                                                  MALLOC_CAP_SPIRAM);
            if (!new_buf) { free(pcm_buf); return ESP_ERR_NO_MEM; }
            pcm_buf = new_buf;
        }
        memcpy(&pcm_buf[total_samples], chunk, chunk_len * sizeof(int16_t));
        total_samples += chunk_len;
    }

    if (total_samples == 0) { free(pcm_buf); return ESP_FAIL; }


    int16_t vmin = 32767, vmax = -32768;
    int64_t vsum = 0;
    size_t check_n = total_samples < 4000 ? total_samples : 4000;
    for (size_t i = 0; i < check_n; i++) {
        if (pcm_buf[i] < vmin) vmin = pcm_buf[i];
        if (pcm_buf[i] > vmax) vmax = pcm_buf[i];
        vsum += (pcm_buf[i] >= 0 ? pcm_buf[i] : -pcm_buf[i]);
    }
    ESP_LOGI(TAG, "PCM diag: samples=%u min=%d max=%d avg=%lld",
             (unsigned)total_samples,
             (int)vmin, (int)vmax, (long long)(vsum / (int64_t)check_n));

    ESP_LOGI(TAG, "TTS: %u samples (%.1fs), playing...",
             (unsigned)total_samples, (double)total_samples / 16000.0);
    esp_err_t ret = i2s_audio_play(pcm_buf, total_samples);
    free(pcm_buf);
    if (ret != ESP_OK) ESP_LOGE(TAG, "I2S play failed: %s", esp_err_to_name(ret));
    return ret;
}


static void tts_task(void *arg)
{
    char *text;
    while (xQueueReceive(s_tts_queue, &text, portMAX_DELAY) == pdTRUE) {
        s_tts_busy = true;
        esp_err_t ret = local_tts_play_sync(text);
        ESP_LOGI(TAG, "TTS finished: %s", esp_err_to_name(ret));
        free(text);
        s_tts_busy = false;
    }
}


esp_err_t local_tts_play(const char *chinese_text)
{
    if (!s_tts_queue) {
        ESP_LOGE(TAG, "Local TTS unavailable; check voice_data flash/init logs");
        return ESP_ERR_INVALID_STATE;
    }
    if (!chinese_text || strlen(chinese_text) == 0) return ESP_ERR_INVALID_ARG;

    char *text_copy = strdup(chinese_text);
    if (!text_copy) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_tts_queue, &text_copy, 0) != pdTRUE) {
        free(text_copy);
        ESP_LOGW(TAG, "TTS queue full; request rejected");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "TTS queued: %s", chinese_text);
    return ESP_OK;
}


esp_err_t local_tts_play_wait(const char *chinese_text)
{
    esp_err_t ret = local_tts_play(chinese_text);
    if (ret != ESP_OK) return ret;
    if (!s_tts_queue) return ESP_ERR_INVALID_STATE;

    int64_t deadline = esp_timer_get_time() + 30LL * 1000 * 1000;
    while (esp_timer_get_time() < deadline) {
        if (!s_tts_busy && uxQueueMessagesWaiting(s_tts_queue) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "TTS wait timeout: %s", chinese_text);
    return ESP_ERR_TIMEOUT;
}
