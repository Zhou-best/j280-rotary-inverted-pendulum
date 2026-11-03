#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_partition.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "i2s_audio.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "model_path.h"
#include "esp_sr_handler.h"

static const char *TAG = "ESP_SR";


#define SR_LED_GPIO  GPIO_NUM_8
#define SR_LED_ON()  gpio_set_level(SR_LED_GPIO, 1)
#define SR_LED_OFF() gpio_set_level(SR_LED_GPIO, 0)


static const char *s_cmd_pinyin[] = {
    "qing jin ru dou bao",
    "qing fan hui shang yi jie mian",
    "qing jin xing qu gan ce qu yun dong",
    "qing jin xing ce ping ju yun dong",
    "qing jin xing qu shen zhou yun dong",
    "qing kai shi ji shu",
    "qing jie shu dang qian yun dong",
    "qing bo bao dang qian ji shu",
    "qing sheng cheng jian shen bao gao",
    "qing jin ru liao tian dui hua",
    "qing chong shu jian shen bao gao",
};

static const char *s_cmd_chinese[] = {
    "请进入豆包", "请返回上一界面",
    "请进行躯干侧屈运动", "请进行侧平举运动", "请进行屈伸肘运动",
    "请开始计数", "请结束当前运动", "请播报当前计数",
    "请生成健身报告", "请进入聊天对话", "请重述健身报告",
};

#define CMD_COUNT (sizeof(s_cmd_pinyin) / sizeof(s_cmd_pinyin[0]))


static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static esp_afe_sr_data_t        *s_afe_data  = NULL;
static srmodel_list_t           *s_models    = NULL;
static afe_config_t             *s_afe_cfg   = NULL;

static const esp_mn_iface_t *s_mn_iface = NULL;
static model_iface_data_t   *s_mn_model = NULL;


static volatile bool s_running   = false;
static QueueHandle_t s_cmd_queue = NULL;


static inline void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}


static esp_mn_error_t *register_commands(void)
{
    esp_mn_commands_clear();
    for (int i = 0; i < CMD_COUNT; i++) {
        int mn_id = i + 1;
        esp_err_t ret = esp_mn_commands_add(mn_id, s_cmd_pinyin[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Cmd #%d '%s' (MN ID %d) rejected by MultiNet",
                     i, s_cmd_pinyin[i], mn_id);
        }
    }

    esp_err_t stop_alias = esp_mn_commands_add(
        VOICE_CMD_QING_JIE_SHU_DANG_QIAN_YUN_DONG + 1, "qing jie shu dang qian dong zuo");
    if (stop_alias != ESP_OK) ESP_LOGW(TAG, "Stop alias rejected by MultiNet");
    return esp_mn_commands_update();
}


esp_err_t esp_sr_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP-SR (local AI on ESP32-S3)...");


    gpio_config_t led_cfg = {
        .pin_bit_mask = BIT64(SR_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    SR_LED_OFF();
    ESP_LOGI(TAG, "Wake LED on GPIO%d (anode→GPIO, active high)", SR_LED_GPIO);


    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num == 0) {
        ESP_LOGW(TAG, "No models found in 'model' partition — "
                      "did you flash the SPIFFS model image?");

    } else {
        ESP_LOGI(TAG, "Model partition loaded: %d files", s_models->num);
    }


    s_afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!s_afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }


    s_afe_cfg->aec_init  = false;
    s_afe_cfg->se_init   = false;
    s_afe_cfg->vad_init  = true;
    s_afe_cfg->agc_init  = true;
    s_afe_cfg->wakenet_init = true;
    s_afe_cfg->afe_perferred_core = 1;
    s_afe_cfg->afe_perferred_priority = 5;
    s_afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    ESP_LOGI(TAG, "AFE config: 1-mic, 16kHz, VAD+AGC+WakeNet");


    s_afe_iface = esp_afe_handle_from_config(s_afe_cfg);
    if (!s_afe_iface) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config returned NULL");
        return ESP_FAIL;
    }
    s_afe_data = s_afe_iface->create_from_config(s_afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create failed — check PSRAM / model files");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFE pipeline created (feed=%d fetch=%d ch=%d %luHz)",
             s_afe_iface->get_feed_chunksize(s_afe_data),
             s_afe_iface->get_fetch_chunksize(s_afe_data),
             s_afe_iface->get_channel_num(s_afe_data),
             (unsigned long)s_afe_iface->get_samp_rate(s_afe_data));


    s_mn_iface = esp_mn_handle_from_name("mn7_cn");
    if (!s_mn_iface) {
        ESP_LOGE(TAG, "MultiNet7 'mn7_cn' model not found in partition");
        s_afe_iface->destroy(s_afe_data);
        s_afe_data = NULL;
        return ESP_FAIL;
    }
    s_mn_model = s_mn_iface->create("mn7_cn", 10000);
    if (!s_mn_model) {
        ESP_LOGE(TAG, "MultiNet7 create failed — OOM?");
        s_afe_iface->destroy(s_afe_data);
        s_afe_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MultiNet7 loaded: %s, %luHz, chunk=%d",
             s_mn_iface->get_language(s_mn_model),
             (unsigned long)s_mn_iface->get_samp_rate(s_mn_model),
             s_mn_iface->get_samp_chunksize(s_mn_model));


    esp_mn_error_t *err = register_commands();
    if (err && err->num > 0) {
        ESP_LOGW(TAG, "%d command(s) failed to add to FST:", err->num);
        for (int i = 0; i < err->num && i < 10; i++) {
            if (err->phrases[i]) {
                ESP_LOGW(TAG, "  REJECTED: MN ID %d '%s'",
                         err->phrases[i]->command_id,
                         err->phrases[i]->string ? err->phrases[i]->string : "?");
            }
        }
    } else {
        ESP_LOGI(TAG, "All %u commands added to FST successfully", (unsigned)CMD_COUNT);
    }
    s_mn_iface->print_active_speech_commands(s_mn_model);
    ESP_LOGI(TAG, "ESP-SR init complete, %u commands registered", (unsigned)CMD_COUNT);

    return ESP_OK;
}


static void sr_feed_task(void *arg)
{
    int feed_chunksize = s_afe_iface->get_feed_chunksize(s_afe_data);
    int feed_nch = s_afe_iface->get_feed_channel_num(s_afe_data);
    size_t feed_samples = (size_t)feed_chunksize * feed_nch;
    if (feed_samples == 0) {
        ESP_LOGE(TAG, "Feed: invalid feed size (chunk=%d ch=%d)",
                 feed_chunksize, feed_nch);
        vTaskDelete(NULL);
        return;
    }


    size_t feed_bytes = feed_samples * sizeof(int16_t);
    size_t filled_bytes = 0;
    int16_t *raw_buf = heap_caps_malloc(feed_bytes, MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        ESP_LOGE(TAG, "Feed: OOM");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Feed task on CPU0 (chunk=%d ch=%d %u samp, MONO LEFT)",
             feed_chunksize, feed_nch, (unsigned)feed_samples);

    extern volatile bool s_i2s_rx_grabbed;
    extern volatile bool s_i2s_tx_active;

    int feed_count = 0;
    int peak = 0;
    int clip_cnt = 0;
    int rx_fail_cnt = 0;
    int raw_dump = 0;


    int timeout_events = 0;
    int timeout_total = 0;
    int32_t max_gap_ms = 0;
    int64_t last_ok_us = 0;
    int64_t last_warn_us = 0;

    while (s_running) {
        if (s_i2s_tx_active) {
            filled_bytes = 0;
            delay_ms(10);
            continue;
        }
        if (s_i2s_rx_grabbed) {
            filled_bytes = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }


        i2s_chan_handle_t rx_chan = i2s_audio_get_rx_channel();
        if (!rx_chan) {
            filled_bytes = 0;

            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }


        if (s_i2s_tx_active) {
            filled_bytes = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytes_read = 0;
        int64_t t_start = esp_timer_get_time();


        esp_err_t ret = i2s_channel_read(rx_chan, (uint8_t *)raw_buf + filled_bytes,
                                         feed_bytes - filled_bytes, &bytes_read, 100);
        int64_t t_end = esp_timer_get_time();
        int read_ms = (int)((t_end - t_start) / 1000);

        if (s_i2s_tx_active || s_i2s_rx_grabbed) {
            filled_bytes = 0;
            continue;
        }

        if ((ret == ESP_OK || ret == ESP_ERR_TIMEOUT) && bytes_read > 0) {

            if (last_ok_us) {
                int32_t gap_ms = (int32_t)((t_start - last_ok_us) / 1000);
                if (gap_ms > max_gap_ms) max_gap_ms = gap_ms;
            }
            last_ok_us = t_end;
            filled_bytes += bytes_read;
            rx_fail_cnt = 0;
            if (filled_bytes == feed_bytes) {
                for (size_t i = 0; i < feed_samples; i++) {
                    int v = raw_buf[i];
                    int magnitude = v < 0 ? -v : v;
                    if (magnitude > peak) peak = magnitude;
                    if (magnitude >= 32000) clip_cnt++;
                }
                s_afe_iface->feed(s_afe_data, raw_buf);
                filled_bytes = 0;
                if (raw_dump < 3) {
                    ESP_LOGI(TAG, "Raw[%d]: %u mono samples [0..3]=%d %d %d %d",
                             raw_dump, (unsigned)feed_samples,
                             (int)raw_buf[0], (int)raw_buf[1],
                             (int)raw_buf[2], (int)raw_buf[3]);
                    raw_dump++;
                }
            }
        } else {
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) filled_bytes = 0;
            rx_fail_cnt++;
            timeout_events++;
            timeout_total++;


            bool first_warn = (timeout_total <= 3);
            bool warn_due = (last_warn_us == 0) ||
                            ((t_end - last_warn_us) >= 5000000);
            if (first_warn || warn_due) {
                last_warn_us = t_end;
                ESP_LOGW(TAG, "I2S RX timeout: ret=%s bytes=%u read=%dms gap=%dms "
                              "filled=%u/%u (events=%d, total=%d)",
                         esp_err_to_name(ret), (unsigned)bytes_read, read_ms,
                         last_ok_us ? (int)((t_start - last_ok_us) / 1000) : -1,
                         (unsigned)filled_bytes, (unsigned)feed_bytes,
                         timeout_events, timeout_total);
            }
        }
        taskYIELD();
        if (++feed_count >= 500) {
            feed_count = 0;
            ESP_LOGI(TAG, "Mic peak: %d (0-32767, >500=OK, <50=DEAD)  "
                          "clip=%d rx_fail=%d to=%d max_gap=%dms",
                     (int)peak, clip_cnt, rx_fail_cnt, timeout_events, (int)max_gap_ms);
            peak = 0;
            clip_cnt = 0;
            timeout_events = 0;
            max_gap_ms = 0;
        }
    }

    free(raw_buf);
    ESP_LOGI(TAG, "Feed task stopped");
    vTaskDelete(NULL);
}


static void sr_detect_task(void *arg)
{


    int mn_duration = 10000;
    bool mn_active = false;
    int64_t mn_deadline = 0;
    int mn_call_count = 0;

    float mn_peak_vol = -100.0f;
    bool  mn_saw_speech = false;

    #define MN_CMD_MAX 1
    int mn_cmd_count = 0;

    ESP_LOGI(TAG, "Detect task on CPU1 (continuous mode, max %d cmds/wake)", MN_CMD_MAX);

    while (s_running) {

        if (s_i2s_tx_active || s_i2s_rx_grabbed) {
            vTaskDelay(1);
            if (mn_active) s_mn_iface->clean(s_mn_model);
            mn_active = false;
            SR_LED_OFF();
            continue;
        }


        afe_fetch_result_t *res = s_afe_iface->fetch(s_afe_data);
        if (res == NULL || res->data_size <= 0) {
            vTaskDelay(1);
            continue;
        }


        static int detect_diag = 0;
        if (++detect_diag % 100 == 0) {
            ESP_LOGI(TAG, "Detect: data_size=%d wakeup=%d",
                     res->data_size, res->wakeup_state);
        }

        if (!mn_active) {

            if (res->wakeup_state == WAKENET_DETECTED) {

                s_mn_iface->clean(s_mn_model);
                ESP_LOGI(TAG, "Wake word detected! Listening (max %d cmds, %.1fs window)...",
                         MN_CMD_MAX, mn_duration / 1000.0);
                SR_LED_ON();
                mn_active = true;
                mn_deadline = esp_timer_get_time() + (int64_t)mn_duration * 1000;
                mn_call_count = 0;
                mn_cmd_count = 0;
                mn_peak_vol = -100.0f;
                mn_saw_speech = false;
            }
            taskYIELD();
        } else {

            if (res->data_volume > mn_peak_vol) mn_peak_vol = res->data_volume;
            if (res->vad_state == VAD_SPEECH) mn_saw_speech = true;
            esp_mn_state_t mn_ret = s_mn_iface->detect(
                s_mn_model, res->data);


            if (mn_ret == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *results = s_mn_iface->get_results(s_mn_model);
                if (results && results->num > 0) {

                    int cmd_idx = results->command_id[0] - 1;
                    ESP_LOGI(TAG, "MultiNet DETECTED: MN_ID=%d idx=%d prob=%.2f "
                                  "(peak_vol=%.0fdB vad=%d)",
                             results->command_id[0], cmd_idx, results->prob[0],
                             mn_peak_vol, mn_saw_speech ? 1 : 0);
                    if (cmd_idx >= 0 && cmd_idx < (int)CMD_COUNT
                        && results->prob[0] >= 0.1f) {
                        voice_cmd_id_t cmd_id = (voice_cmd_id_t)cmd_idx;
                        mn_cmd_count++;
                        ESP_LOGI(TAG, "Voice cmd [%d/%d]: [%d] %s (prob=%.2f) → QUEUE",
                                 mn_cmd_count, MN_CMD_MAX,
                                 cmd_idx, s_cmd_chinese[cmd_idx],
                                 results->prob[0]);
                        BaseType_t qret = xQueueSend(s_cmd_queue, &cmd_id, 0);
                        ESP_LOGI(TAG, "xQueueSend: %s",
                                 qret == pdTRUE ? "OK" : "FULL");

                        if (mn_cmd_count >= MN_CMD_MAX) {
                            ESP_LOGI(TAG, "Max commands (%d/%d) reached → wake mode",
                                     mn_cmd_count, MN_CMD_MAX);
                            SR_LED_OFF();
                            s_mn_iface->clean(s_mn_model);
                            mn_active = false;
                        }

                    } else if (cmd_idx >= 0 && cmd_idx < (int)CMD_COUNT) {
                        ESP_LOGI(TAG, "Low-confidence cmd [%d] %s prob=%.2f < 0.1, ignored",
                                 cmd_idx, s_cmd_chinese[cmd_idx],
                                 results->prob[0]);
                    } else {
                        ESP_LOGW(TAG, "Detected unknown cmd MN_ID: %d (idx=%d out of 0-%d)",
                                 results->command_id[0], cmd_idx, (int)CMD_COUNT - 1);
                    }
                }
            }
            else if (mn_ret == ESP_MN_STATE_TIMEOUT) {
                ESP_LOGI(TAG, "MultiNet timeout (%d cmds consumed) → wake mode",
                         mn_cmd_count);
                SR_LED_OFF();
                s_mn_iface->clean(s_mn_model);
                mn_active = false;
            }
            else if (esp_timer_get_time() > mn_deadline) {
                ESP_LOGI(TAG, "Deadline exceeded (%d cmds consumed) → wake mode "
                              "(peak_vol=%.0fdB vad=%d)",
                         mn_cmd_count, mn_peak_vol, mn_saw_speech ? 1 : 0);
                SR_LED_OFF();
                s_mn_iface->clean(s_mn_model);
                mn_active = false;
            }

            if (++mn_call_count % 100 == 0) {
                ESP_LOGI(TAG, "Listening... (cmd %d/%d, remain=%.1fs, vol=%.0fdB vad=%d)",
                         mn_cmd_count, MN_CMD_MAX,
                         (double)(mn_deadline - esp_timer_get_time()) / 1000000.0,
                         res->data_volume, res->vad_state);
            }


            static int yield_div = 0;
            if (++yield_div >= 8) {
                yield_div = 0;
                vTaskDelay(1);
            } else {
                taskYIELD();
            }
        }
    }

    ESP_LOGI(TAG, "Detect task stopped");
    vTaskDelete(NULL);
}


esp_err_t esp_sr_handler_start(QueueHandle_t cmd_queue)
{
    if (!s_afe_data || !s_mn_model) {
        ESP_LOGE(TAG, "Not initialized — call esp_sr_handler_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_cmd_queue = cmd_queue;
    s_running = true;

    BaseType_t r1 = xTaskCreatePinnedToCore(
        sr_feed_task, "sr_feed", 8192, NULL, 8, NULL, 0);
    if (r1 != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sr_feed task");
        s_running = false;
        return ESP_FAIL;
    }


    BaseType_t r2 = xTaskCreatePinnedToCore(
        sr_detect_task, "sr_detect", 16384, NULL, 5, NULL, 1);
    if (r2 != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sr_detect task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice recognition active (feed CPU0 + detect CPU1)");
    return ESP_OK;
}


esp_err_t esp_sr_handler_stop(void)
{
    s_running = false;
    SR_LED_OFF();
    delay_ms(300);

    if (s_mn_model && s_mn_iface) {
        s_mn_iface->destroy(s_mn_model);
        s_mn_model = NULL;
    }
    if (s_afe_data && s_afe_iface) {
        s_afe_iface->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    if (s_afe_cfg) {
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
    }
    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    s_cmd_queue = NULL;
    ESP_LOGI(TAG, "ESP-SR stopped, all resources freed");
    return ESP_OK;
}


const char *esp_sr_get_cmd_text(voice_cmd_id_t cmd_id)
{
    if (cmd_id < 0 || cmd_id >= VOICE_CMD_COUNT) return "?";
    return s_cmd_chinese[cmd_id];
}
