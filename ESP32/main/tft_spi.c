#include "tft_spi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "local_tts.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "blueberry_icon.h"
#include "chinese_font.h"
#include "uart_startup.h"
#include "k230_tcp.h"
#include "espnow_comm.h"
#include "ai_chat.h"
#include "ai_asr.h"
#include "i2s_audio.h"
#include "wifi.h"
#include "esp_sr_handler.h"
#define DRAW_PROTECT_UART_PORT   UART_NUM_1

static const char *TAG = "TFT";


static void log_mem(const char *stage)
{
    ESP_LOGI(TAG, "MEM[%s]: DRAM free %uK, PSRAM free %uK", stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}


#define CLOUD_TTS_RATE  24000
spi_device_handle_t disp_spi = NULL;


#define THEME_BG         0x02C5
#define THEME_TEXT       COLOR_YELLOW
#define THEME_LINE       0x6E2C
#define THEME_HIGHLIGHT  0xFFE0


#define AUTO_SCROLL_DEMO     0
#define SCROLL_WELCOME_MS    3000
#define SCROLL_SELECT_MS     3000
#define SCROLL_MONITOR_MS    6000


#define VOICE_ALLOW_START_EXERCISE   1


typedef enum {
    SCREEN_WELCOME = 0,
    SCREEN_SELECT,
    SCREEN_MONITOR,
    SCREEN_AI_CHAT
} screen_t;

static screen_t current_screen = SCREEN_WELCOME;
static uint32_t exercise_count = 0;
static uint32_t qualified_count = 0;
static uint32_t unqualified_count = 0;
static bool exercise_active = false;


typedef enum {
    EXERCISE_NONE = 0,
    EXERCISE_EXPAND_CHEST,
    EXERCISE_SIDE_RAISE,
    EXERCISE_ELBOW_CURL
} exercise_t;

static exercise_t current_exercise = EXERCISE_NONE;


extern QueueHandle_t g_voice_cmd_queue;


static volatile bool s_ai_running = false;
static volatile bool s_ai_ok      = false;
static volatile bool s_ptt_running = false;

static char s_ptt_status[512] = {0};
static volatile bool s_ptt_status_dirty = false;
static portMUX_TYPE s_ai_status_lock = portMUX_INITIALIZER_UNLOCKED;


static volatile bool s_redraw_pending = false;
void tft_request_redraw(void) { s_redraw_pending = true; }


static volatile int s_k230_ui_cmd = -1;
static volatile int s_k230_ui_action = -1;
void tft_k230_ui_request(int cmd, int action)
{
    s_k230_ui_action = action;
    s_k230_ui_cmd = cmd;
}


void tft_web_ui_request(int cmd, int action)
{
    if (cmd == 5) action = -1;
    s_k230_ui_action = action;
    s_k230_ui_cmd = cmd;
}


static void ai_status(const char *msg) {
    size_t len = strlen(msg);
    if (len >= sizeof(s_ptt_status)) {
        len = sizeof(s_ptt_status) - 1;
        while (len && ((unsigned char)msg[len] & 0xc0) == 0x80) len--;
    }
    taskENTER_CRITICAL(&s_ai_status_lock);
    memcpy(s_ptt_status, msg, len);
    s_ptt_status[len] = '\0';
    s_ptt_status_dirty = true;
    taskEXIT_CRITICAL(&s_ai_status_lock);
}


#define REPORT_CACHE_SIZE 1024
static char *s_last_report_text = NULL;
static portMUX_TYPE s_last_report_lock = portMUX_INITIALIZER_UNLOCKED;

static bool report_cache_ensure(void)
{
    if (s_last_report_text) return true;
    char *buf = heap_caps_malloc(REPORT_CACHE_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "Report cache unavailable (PSRAM OOM)");
        return false;
    }
    taskENTER_CRITICAL(&s_last_report_lock);
    if (!s_last_report_text) {
        s_last_report_text = buf;
        buf = NULL;
    }
    taskEXIT_CRITICAL(&s_last_report_lock);
    if (buf) free(buf);
    return s_last_report_text != NULL;
}


static void report_copy_bounded(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void report_cache_text(const char *text)
{
    if (!text || !text[0] || !report_cache_ensure()) return;
    taskENTER_CRITICAL(&s_last_report_lock);
    report_copy_bounded(s_last_report_text, text, REPORT_CACHE_SIZE);
    taskEXIT_CRITICAL(&s_last_report_lock);
    ESP_LOGI(TAG, "Report cached for replay (%u bytes)", (unsigned)strlen(text));
}


static bool report_copy_last(char *out, size_t out_size)
{
    bool found = false;
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    taskENTER_CRITICAL(&s_last_report_lock);
    if (s_last_report_text && s_last_report_text[0]) {
        report_copy_bounded(out, s_last_report_text, out_size);
        found = true;
    }
    taskEXIT_CRITICAL(&s_last_report_lock);
    return found;
}


typedef enum {
    REPORT_JOB_GENERATE = 1,
    REPORT_JOB_REPLAY   = 2,
} report_job_t;

static QueueHandle_t s_report_job_q = NULL;
static TaskHandle_t  s_report_worker = NULL;
static k230_status_t  s_report_job_status;
static char          *s_report_job_text = NULL;

static void report_do_generate(void)
{
    k230_status_t status = s_report_job_status;
    s_ai_ok = false;
    if (!is_wifi_connected()) {
        ai_status("网络已断开，无法生成报告");
        return;
    }
    char prompt[1280];
    const char *name = status.action == 0 ? "躯干侧屈" : status.action == 2 ? "屈伸肘" : "侧平举";
    const char *errors = status.action == 0 ?
        "幅度不足，骨盆偏移，预留，预留，幅度过大，预留" :
        status.action == 2 ?
        "左肘屈曲不足，右肘屈曲不足，双臂不同步，上臂抬起，预留，预留" :
        "左臂低，右臂低，左右不齐，肘弯曲，抬过高，未展开";
    snprintf(prompt, sizeof(prompt),
             "基于视觉估计的%s%s报告：完成%d次，合格%d次，不合格%d次，"
             "未完成尝试%d次，跟踪丢失或结束打断%d次。完成动作平均最高分%d。"
             "错误动作次数：%s。对应次数依次是%d，%d，%d，%d，%d，%d；未达到合格保持时间%d。"
             "同一动作可能有多个错误，不可把错误数相加作动作次数。"
             "未完成/打断不包含在完成次数中。只按数据总结，零次不推断能力，"
             "不作医疗诊断，用100字以内的纯中文短文给出改进建议及鼓励，"
             "必须说完整句子，最后一句要有句号，不使用Markdown。",
             name, status.running ? "训练中" : "已结束",
             status.total, status.standard, status.nonstandard,
             status.incomplete, status.interrupted,
             status.total ? status.score_sum / status.total : 0,
             errors,
             status.errors[1], status.errors[2], status.errors[3], status.errors[4],
             status.errors[8], status.errors[9], status.short_hold);

    ai_status("正在生成健身报告");
    const char *error = NULL;
    char *reply = ai_chat_text_only_with_error(prompt, &error);
    if (!reply) {
        ai_status(error ? error : "报告生成失败，请查看串口HTTP/API错误");
    } else {

        s_ai_ok = true;
        ESP_LOGI(TAG, "FITNESS_REPORT session=%s: %s", status.session, reply);

        report_cache_text(reply);
        ai_status("报告已生成，正在准备播报");
        size_t samples = 0;
        int16_t *pcm = ai_tts_synthesize(reply, &samples);
        esp_err_t play_ret = pcm ? i2s_audio_play_rate(pcm, samples, CLOUD_TTS_RATE) : ESP_FAIL;
        free(pcm);
        const char *result = "报告已生成：";
        if (play_ret != ESP_OK) {
            esp_err_t fallback = local_tts_play(reply);
            result = fallback == ESP_OK ? "报告已生成，本地语音已排队：" : "报告已生成，语音失败：";
            ESP_LOGW(TAG, "Report cloud playback failed; local fallback=%s", esp_err_to_name(fallback));
        }
        char message[2048];
        snprintf(message, sizeof(message), "%s%s", result, reply);
        ai_status(message);
        free(reply);
    }
}


static void report_do_replay(void)
{
    char *text = s_report_job_text;
    s_report_job_text = NULL;
    if (!text) return;
    s_ai_ok = false;
    if (!is_wifi_connected()) {
        ai_status("网络已断开，正在用本地语音重述");
        if (local_tts_play(text) != ESP_OK) ai_status("网络已断开，无法重述报告");
        free(text);
        return;
    }
    ai_status("正在重述健身报告");
    size_t samples = 0;
    int16_t *pcm = ai_tts_synthesize(text, &samples);
    esp_err_t play_ret = pcm ? i2s_audio_play_rate(pcm, samples, CLOUD_TTS_RATE) : ESP_FAIL;
    free(pcm);
    const char *result = "报告重述完成：";
    if (play_ret != ESP_OK) {
        esp_err_t fallback = local_tts_play(text);
        result = fallback == ESP_OK ? "报告重述完成，本地语音已排队：" : "报告重述失败：";
        ESP_LOGW(TAG, "Report replay cloud playback failed; local fallback=%s", esp_err_to_name(fallback));
    }
    char message[1024];
    snprintf(message, sizeof(message), "%s%s", result, text);
    ai_status(message);
    free(text);
}


static void report_worker_task(void *arg)
{
    report_job_t job;
    for (;;) {
        if (xQueueReceive(s_report_job_q, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job == REPORT_JOB_GENERATE) {
            report_do_generate();
        } else if (job == REPORT_JOB_REPLAY) {
            report_do_replay();
        }
        s_ai_running = false;
    }
}


typedef struct { int16_t *pcm; size_t samples; } ai_pcm_t;


#define AI_MAX_SENTENCES 16
static char *s_sentences[AI_MAX_SENTENCES];
static int   s_sentence_count = 0;


static ai_pcm_t s_pcm_items[AI_MAX_SENTENCES];
static int s_pcm_count = 0;


static void trim_clip_silence(int16_t *pcm, size_t *samples)
{
    const int16_t thr = 300;
    size_t n = *samples;
    if (n < 64) return;
    size_t a = 0;
    while (a < n && pcm[a] < thr && pcm[a] > -thr) a++;
    if (a == n) { *samples = 0; return; }
    size_t b = n;
    while (b > a && pcm[b - 1] < thr && pcm[b - 1] > -thr) b--;
    a = (a > 32) ? a - 32 : 0;
    b = (b + 32 < n) ? b + 32 : n;
    size_t len = b - a;
    if (len < 800) return;
    memmove(pcm, pcm + a, len * sizeof(int16_t));
    *samples = len;
}

static void ai_sentence_collect(const char *sentence, void *user)
{
    (void)user;
    if (!sentence || !sentence[0]) return;
    if (s_sentence_count >= AI_MAX_SENTENCES) return;
    size_t n = strlen(sentence);
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!copy) return;
    memcpy(copy, sentence, n + 1);
    s_sentences[s_sentence_count++] = copy;
}

static void ai_stream_and_synthesize(char *question)
{
    s_sentence_count = 0;
    char *reply = ai_chat_stream(question, ai_sentence_collect, NULL);
    free(question);
    if (reply) {
        ESP_LOGI(TAG, "AI reply (%u bytes): %s", (unsigned)strlen(reply), reply);
        free(reply);
    } else {
        ESP_LOGE(TAG, "Streaming chat failed");
    }


    s_pcm_count = 0;
    for (int i = 0; i < s_sentence_count; i++) {
        char *sentence = s_sentences[i];
        char buf[200];
        snprintf(buf, sizeof(buf), "AI：%.165s", sentence);
        ai_status(buf);

        size_t samples = 0;
        int16_t *pcm = ai_tts_synthesize(sentence, &samples);
        if (pcm && samples > 0) {
            trim_clip_silence(pcm, &samples);
            if (samples > 0 && s_pcm_count < AI_MAX_SENTENCES) {
                s_pcm_items[s_pcm_count].pcm = pcm;
                s_pcm_items[s_pcm_count].samples = samples;
                s_pcm_count++;
            } else {
                free(pcm);
            }
        } else if (sentence[0]) {

            ESP_LOGW(TAG, "Cloud TTS failed, sentence skipped: %.40s", sentence);
        }
        free(sentence);
        s_sentences[i] = NULL;
    }
    s_sentence_count = 0;
}


static void ptt_task(void *arg)
{

    if (s_ptt_running) { vTaskDelete(NULL); return; }
    s_ptt_running = true;


    bool handsfree = (arg != NULL);


    if (!is_wifi_connected()) {
        ai_status("未连接网络，无法对话");
        local_tts_play("未连接网络，请检查路由器");
        s_ptt_running = false;
        vTaskDelete(NULL);
        return;
    }

    extern volatile bool s_i2s_tx_active;
    if (s_i2s_tx_active) {
        ai_status("播放中，请稍候再试");
        s_ptt_running = false;
        vTaskDelete(NULL);
        return;
    }

#define MAX_RECORD_SAMPLES (16000 * 6)

    int16_t *pcm = heap_caps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM);
    if (!pcm) {
        ai_status("内存不足，无法录音");
        s_ptt_running = false;
        vTaskDelete(NULL);
        return;
    }


    int64_t t_grab = esp_timer_get_time();
    i2s_audio_grab_rx();
    ESP_LOGI(TAG, "PTT: I2S RX grabbed in %lld ms",
             (long long)((esp_timer_get_time() - t_grab) / 1000));
    log_mem("before ASR");

    ai_status(handsfree ? "请说话…" : "按住A说话…");
    ESP_LOGI(TAG, "PTT: recording start (%s, A pressed=%d)",
             handsfree ? "hands-free" : "button", button_a_pressed());


    size_t recorded = 0;
    int64_t deadline_us = esp_timer_get_time() + 6 * 1000 * 1000;
    int empty_reads = 0;
    int speech_seen = 0, silence_chunks = 0;
    while (recorded < MAX_RECORD_SAMPLES
           && esp_timer_get_time() < deadline_us) {
        size_t to_read = (MAX_RECORD_SAMPLES - recorded) < 512
                         ? (MAX_RECORD_SAMPLES - recorded) : 512;
        size_t start = recorded;
        size_t n = i2s_audio_read_chunk(&pcm[recorded], to_read, 200);
        if (n > 0) {
            recorded += n;
            empty_reads = 0;
            if (handsfree) {
                int peak = 0;
                for (size_t k = start; k < recorded; k++) {
                    int v = pcm[k] < 0 ? -pcm[k] : pcm[k];
                    if (v > peak) peak = v;
                }
                if (peak > 1200) { speech_seen = 1; silence_chunks = 0; }
                else if (speech_seen) silence_chunks++;
            }
        }
        else { if (++empty_reads >= 5) break; }

        if (!handsfree) {
            if (recorded >= 16000 && !button_a_pressed()) break;
        } else if (recorded >= 16000 && speech_seen && silence_chunks >= 60) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int64_t t_record_end = esp_timer_get_time();


    i2s_audio_release_rx();
    ESP_LOGI(TAG, "PTT: recorded %u samples (%.1fs), RX released",
             (unsigned)recorded, (double)recorded / 16000.0);


    {
        int ptt_peak = 0;
        int64_t ptt_abs = 0;
        size_t ptt_zero = 0;
        for (size_t i = 0; i < recorded; i++) {
            int v = pcm[i];
            int a = v < 0 ? -v : v;
            if (a > ptt_peak) ptt_peak = a;
            ptt_abs += a;
            if (v == 0) ptt_zero++;
        }
        ESP_LOGI(TAG, "PTT PCM: peak=%d mean_abs=%d zero=%u/%u first=%d,%d,%d,%d",
                 ptt_peak, recorded ? (int)(ptt_abs / (int64_t)recorded) : 0,
                 (unsigned)ptt_zero, (unsigned)recorded,
                 recorded > 3 ? pcm[0] : 0, recorded > 3 ? pcm[1] : 0,
                 recorded > 3 ? pcm[2] : 0, recorded > 3 ? pcm[3] : 0);
        if (ptt_peak < 300) {
            ESP_LOGW(TAG, "PTT PCM looks silent; check INMP441 / I2S RX before ASR");
        }
    }


    if (recorded < 24000) {
        ai_status("说话太短 A:重试 C:返回");
        free(pcm); s_ptt_running = false; vTaskDelete(NULL); return;
    }


    ai_status("识别中…");
    ESP_LOGI(TAG, "PTT: calling ASR (%u samples)...", (unsigned)recorded);
    char *question = ai_asr_recognize(pcm, recorded);
    int64_t t_asr = esp_timer_get_time();
    ESP_LOGI(TAG, "TIMING: record=%.2fs asr=%.2fs",
             (double)(t_record_end - t_grab) / 1e6,
             (double)(t_asr - t_record_end) / 1e6);
    log_mem("after ASR");
    free(pcm);
    ESP_LOGI(TAG, "PTT: ASR returned %s",
             question ? (question[0] ? question : "(empty)") : "NULL");

    if (!question) {
        if (ai_asr_last_status() == AI_ASR_STATUS_BUSY) {
            ai_status("识别服务繁忙，请等 20 秒再试");
            local_tts_play("识别服务繁忙，请等二十秒再试");
        } else {
            ai_status("识别失败 A:重试 C:返回");
        }
        s_ptt_running = false; vTaskDelete(NULL); return;
    }
    if (question[0] == '\0') {
        ai_status("未听到语音 A:重试 C:返回");
        free(question); s_ptt_running = false; vTaskDelete(NULL); return;
    }

    ESP_LOGI(TAG, "PTT recognized: \"%s\"", question);


    {
        char buf[160];
        snprintf(buf, sizeof(buf), "我说：%s", question);
        ai_status(buf);
    }


    ESP_LOGI(TAG, "PTT: starting streaming chat...");
    ai_status("AI 正在回答…");
    ai_stream_and_synthesize(question);
    question = NULL;


    int64_t t_first_audio = esp_timer_get_time();
    int64_t t_end = 0;
    int sentence_count = s_pcm_count;
    if (s_pcm_count > 0) {
        const int16_t *ptrs[AI_MAX_SENTENCES];
        size_t counts[AI_MAX_SENTENCES];
        for (int i = 0; i < s_pcm_count; i++) {
            ptrs[i] = s_pcm_items[i].pcm;
            counts[i] = s_pcm_items[i].samples;
        }
        ESP_LOGI(TAG, "TIMING: first audio after %.2fs (from key release)",
                 (double)(t_first_audio - t_record_end) / 1e6);
        i2s_audio_play_multi(ptrs, counts, s_pcm_count, CLOUD_TTS_RATE);
    }
    for (int i = 0; i < s_pcm_count; i++) {
        free(s_pcm_items[i].pcm);
        s_pcm_items[i].pcm = NULL;
    }
    s_pcm_count = 0;

    t_end = esp_timer_get_time();
    log_mem("after TTS");

    ESP_LOGI(TAG, "TIMING: sentences=%d total=%.2fs (from key release)",
             sentence_count, (double)(t_end - t_record_end) / 1e6);

    ai_status("完成 A:继续 C:返回");
    ESP_LOGI(TAG, "PTT: complete");
    s_ptt_running = false;
    vTaskDelete(NULL);
}


static const uint8_t ascii_font_8x16[][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    {0,0x66,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0,0,0,0},
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0,0},
    {0,0,0,0,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0,0,0,0},
    {0,0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    {0,0x30,0x30,0x30,0x60,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0},
    {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0},
    {0,0,0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0},
    {0,0,0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0x18,0x18,0x18,0x30,0,0,0},
    {0,0,0,0,0,0,0,0xFE,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0},
    {0,0,0,0,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0,0,0,0},
    {0,0,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0,0,0,0},
    {0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0},
    {0,0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0,0,0,0},
    {0,0,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
    {0,0,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0,0,0,0},
    {0,0,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
    {0,0,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0,0,0,0},
    {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0},
    {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0},
    {0,0,0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0},
    {0,0,0,0,0,0x7E,0,0,0x7E,0,0,0,0,0,0,0},
    {0,0,0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    {0,0,0,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0,0,0,0},
    {0,0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    {0,0,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0,0,0,0},
    {0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0,0,0,0},
    {0,0,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0,0,0,0},
    {0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0,0,0,0},
    {0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0,0,0,0},
    {0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0,0,0,0},
    {0,0,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    {0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0,0,0,0},
    {0,0,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0,0,0,0},
    {0,0,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0,0,0,0},
    {0,0,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0,0},
    {0,0,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0,0},
    {0,0,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0,0,0,0},
    {0,0,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0,0,0,0},
    {0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0,0,0,0},
    {0,0,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0,0,0,0},
    {0,0,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    {0,0,0xFF,0xC3,0x86,0x0C,0x18,0x30,0x60,0xC1,0xC3,0xFF,0,0,0,0},
    {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0},
    {0,0,0,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0,0,0,0},
    {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0},
    {0x10,0x38,0x6C,0xC6,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0xFF,0,0},
    {0x30,0x30,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    {0,0,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0,0,0,0},
    {0,0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
    {0,0,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    {0,0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
    {0,0,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0,0,0,0},
    {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0},
    {0,0,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0,0,0,0},
    {0,0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    {0,0,0x06,0x06,0,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0},
    {0,0,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0,0,0,0},
    {0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    {0,0,0,0,0,0xE6,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0,0,0,0},
    {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0,0,0,0},
    {0,0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0},
    {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0},
    {0,0,0,0,0,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0,0,0,0},
    {0,0,0,0,0,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0,0,0,0},
    {0,0,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0,0,0,0},
    {0,0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    {0,0,0,0,0,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0,0,0,0},
    {0,0,0,0,0,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0,0,0,0},
    {0,0,0,0,0,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0,0,0,0},
    {0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0},
    {0,0,0,0,0,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0,0,0,0},
    {0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0,0,0,0},
    {0,0,0x18,0x18,0x18,0x18,0,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    {0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0,0,0,0},
    {0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
};


static volatile uint32_t s_spi_err_count = 0;
static esp_err_t tft_xmit(spi_transaction_t *t)
{
    esp_err_t r = spi_device_transmit(disp_spi, t);
    if (r != ESP_OK) {
        s_spi_err_count++;
        if (s_spi_err_count <= 5 || (s_spi_err_count % 100) == 0) {
            ESP_LOGE(TAG, "SPI xmit failed: %s len=%u (total %u)",
                     esp_err_to_name(r), (unsigned)t->length,
                     (unsigned)s_spi_err_count);
        }
    }
    return r;
}

static void tft_write_cmd(uint8_t cmd) {
    gpio_set_level(TFT_DC, 0);
    gpio_set_level(TFT_CS, 0);
    spi_transaction_t t = { .length = 8, .tx_data = {cmd}, .flags = SPI_TRANS_USE_TXDATA };
    tft_xmit(&t);
    gpio_set_level(TFT_CS, 1);
}
static void tft_write_data(uint8_t data) {
    gpio_set_level(TFT_DC, 1);
    gpio_set_level(TFT_CS, 0);
    spi_transaction_t t = { .length = 8, .tx_data = {data}, .flags = SPI_TRANS_USE_TXDATA };
    tft_xmit(&t);
    gpio_set_level(TFT_CS, 1);
}
static void tft_write_data16(uint16_t data) {
    uint8_t buf[2] = {(uint8_t)(data >> 8), (uint8_t)(data & 0xFF)};
    gpio_set_level(TFT_DC, 1);
    gpio_set_level(TFT_CS, 0);
    spi_transaction_t t = { .length = 16, .tx_buffer = buf, .flags = 0 };
    tft_xmit(&t);
    gpio_set_level(TFT_CS, 1);
}
#define FLIP_180

static void tft_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
#ifdef FLIP_180
    uint16_t tx1 = TFT_WIDTH - 1 - x2;
    uint16_t tx2 = TFT_WIDTH - 1 - x1;
    uint16_t ty1 = TFT_HEIGHT - 1 - y2;
    uint16_t ty2 = TFT_HEIGHT - 1 - y1;
    x1 = tx1; x2 = tx2; y1 = ty1; y2 = ty2;
#endif
    tft_write_cmd(0x2A); tft_write_data16(x1); tft_write_data16(x2);
    tft_write_cmd(0x2B); tft_write_data16(y1); tft_write_data16(y2);
    tft_write_cmd(0x2C);
}


void tft_init(void) {
    ESP_LOGI(TAG, "Init display");


    gpio_set_direction(TFT_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_LED, 0);
    gpio_set_direction(TFT_CS, GPIO_MODE_OUTPUT); gpio_set_direction(TFT_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(TFT_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_CS, 1); gpio_set_level(TFT_DC, 1);
    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_MOSI, .miso_io_num = -1, .sclk_io_num = TFT_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * TFT_HEIGHT * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t devcfg = {


        .clock_speed_hz = 2 * 1000 * 1000, .mode = 3,
        .spics_io_num = -1, .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &disp_spi));
    gpio_set_level(TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));


    tft_write_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    tft_write_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));

    tft_write_cmd(0xF0); tft_write_data(0xC3);
    tft_write_cmd(0xF0); tft_write_data(0x96);
    tft_write_cmd(0x36); tft_write_data(0x28);
    tft_write_cmd(0x3A); tft_write_data(0x55);
    tft_write_cmd(0xB4); tft_write_data(0x01);
    tft_write_cmd(0xB6); tft_write_data(0x80); tft_write_data(0x02); tft_write_data(0x3B);
    tft_write_cmd(0xE8);
    tft_write_data(0x40); tft_write_data(0x8A); tft_write_data(0x00); tft_write_data(0x00);
    tft_write_data(0x29); tft_write_data(0x19); tft_write_data(0xA5); tft_write_data(0x33);
    tft_write_cmd(0xC1); tft_write_data(0x06);
    tft_write_cmd(0xC2); tft_write_data(0x15);
    tft_write_cmd(0xC5); tft_write_data(0x00); tft_write_data(0x28);
    tft_write_cmd(0xE0);
    tft_write_data(0xF0); tft_write_data(0x09); tft_write_data(0x0B); tft_write_data(0x06);
    tft_write_data(0x04); tft_write_data(0x15); tft_write_data(0x2F); tft_write_data(0x54);
    tft_write_data(0x42); tft_write_data(0x3C); tft_write_data(0x17); tft_write_data(0x14);
    tft_write_data(0x18); tft_write_data(0x1B);
    tft_write_cmd(0xE1);
    tft_write_data(0xE0); tft_write_data(0x09); tft_write_data(0x0B); tft_write_data(0x06);
    tft_write_data(0x04); tft_write_data(0x03); tft_write_data(0x2B); tft_write_data(0x43);
    tft_write_data(0x42); tft_write_data(0x3C); tft_write_data(0x16); tft_write_data(0x14);
    tft_write_data(0x17); tft_write_data(0x1B);
    tft_write_cmd(0xF0); tft_write_data(0x3C);
    tft_write_cmd(0xF0); tft_write_data(0x69);

    tft_write_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));
    tft_fill_screen(THEME_BG);


    tft_fill_screen(THEME_BG);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(TFT_LED, 1);
    ESP_LOGI(TAG, "Display OK (ST7796S init, backlight on after clear)");
}


static DMA_ATTR uint8_t tft_line_buf[TFT_WIDTH * 2];

void tft_fill_screen(uint16_t color) {
    tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}
void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    tft_set_window(x, y, x, y);
    tft_write_data16(color);
}
void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    if (x + w > TFT_WIDTH) w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w == 0 || h == 0) return;
    uint8_t ch = color >> 8, cl = color & 0xFF;
    for (int i = 0; i < sizeof(tft_line_buf); i += 2) { tft_line_buf[i] = ch; tft_line_buf[i+1] = cl; }
    spi_transaction_t t = { .tx_buffer = tft_line_buf, .flags = 0 };


    #define TFT_FILL_BAND_ROWS  1
    for (uint16_t y0 = 0; y0 < h; y0 += TFT_FILL_BAND_ROWS) {
        uint16_t bh = h - y0;
        if (bh > TFT_FILL_BAND_ROWS) bh = TFT_FILL_BAND_ROWS;
        tft_set_window(x, y + y0, x + w - 1, y + y0 + bh - 1);

        esp_rom_delay_us(20);
        gpio_set_level(TFT_DC, 1);
        gpio_set_level(TFT_CS, 0);
        uint32_t total = (uint32_t)w * bh * 2;
        for (uint32_t i = 0; i < total; i += sizeof(tft_line_buf)) {
            t.length = (total - i >= sizeof(tft_line_buf)) ? sizeof(tft_line_buf) * 8 : (total - i) * 8;
            if (tft_xmit(&t) != ESP_OK) tft_xmit(&t);
        }
        gpio_set_level(TFT_CS, 1);
    }
    #undef TFT_FILL_BAND_ROWS
}
void tft_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    tft_fill_rect(x, y, w, 1, color);
    tft_fill_rect(x, y + h - 1, w, 1, color);
    tft_fill_rect(x, y, 1, h, color);
    tft_fill_rect(x + w - 1, y, 1, h, color);
}


static void tft_draw_mono(uint16_t x, uint16_t y, const uint8_t *glyph,
                          unsigned glyph_width, uint16_t color, uint16_t bg, uint8_t scale) {
    if (scale == 0 || x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    unsigned w = glyph_width * scale, h = 16 * scale;
    if (w > TFT_WIDTH - x) w = TFT_WIDTH - x;
    if (h > TFT_HEIGHT - y) h = TFT_HEIGHT - y;
    tft_set_window(x, y, x + w - 1, y + h - 1);
    gpio_set_level(TFT_CS, 0); gpio_set_level(TFT_DC, 1);
    spi_transaction_t t = { .tx_buffer = tft_line_buf, .length = w * 16 };
    for (unsigned row = 0; row < h; row++) {
        for (unsigned col = 0; col < w; col++) {
            unsigned gy = row / scale, gx = col / scale;
#ifdef FLIP_180

            gy = (h - 1 - row) / scale; gx = (w - 1 - col) / scale;
#endif
            bool ink = glyph_width == 8 ? (glyph[gy] & (0x80 >> gx)) != 0
                : (glyph[gy * 2 + gx / 8] & (1 << (gx % 8))) == 0;
            uint16_t px = ink ? color : bg;
            tft_line_buf[col * 2] = px >> 8;
            tft_line_buf[col * 2 + 1] = px & 0xff;
        }
        tft_xmit(&t);
    }
    gpio_set_level(TFT_CS, 1);
}

void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
    if (c < 0x20 || c > 0x7E) c = ' ';
    tft_draw_mono(x, y, ascii_font_8x16[c - 0x20], 8, color, bg, scale);
}
void tft_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
    while (*str) { tft_draw_char(x, y, *str, color, bg, scale); x += 8 * scale; str++; }
}


static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;

    if (s[0] < 0x80) { *p += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        *p += 2; return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *p += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *p += 4;
        return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
             | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *p += 1;
    return 0xFFFD;
}


static int font16_lookup(uint32_t cp)
{
    int lo = 0, hi = g_font16_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_font16_cp[mid] == cp) return mid;
        if (g_font16_cp[mid] < cp) lo = mid + 1;
        else                       hi = mid - 1;
    }
    return -1;
}


static void draw_missing16(uint16_t x, uint16_t y, uint16_t color, uint8_t scale)
{
    uint16_t w = 16 * scale;
    tft_draw_rect(x + scale, y + scale, w - 2 * scale, w - 2 * scale, color);
}


static int draw_cp(uint16_t x, uint16_t y, uint32_t cp, uint16_t color, uint16_t bg, uint8_t scale)
{
    if (cp < 0x80) {
        tft_draw_char(x, y, (char)cp, color, bg, scale);
        return 8 * scale;
    }
    int idx = font16_lookup(cp);
    if (idx >= 0) tft_draw_mono(x, y, g_font16[idx], 16, color, bg, scale);
    else {
        tft_fill_rect(x, y, 16 * scale, 16 * scale, bg);
        draw_missing16(x, y, color, scale);
    }
    return 16 * scale;
}


void tft_draw_utf8(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale)
{
    if (scale == 0 || x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    while (*str && x < TFT_WIDTH) {
        uint32_t cp = utf8_next(&str);
        if (cp == 0) break;
        x += draw_cp(x, y, cp, color, bg, scale);
    }
}


int tft_utf8_width(const char *str, uint8_t scale)
{
    int w = 0;
    while (*str) {
        uint32_t cp = utf8_next(&str);
        if (cp == 0) break;
        w += (cp < 0x80) ? 8 * scale : 16 * scale;
    }
    return w;
}


int tft_draw_utf8_wrapped(uint16_t x, uint16_t y, uint16_t max_w, uint8_t max_lines,
                          const char *str, uint16_t color, uint16_t bg, uint8_t scale)
{
    const int line_h  = 16 * scale + 4;
    const int ell_w   = 16 * scale;
    uint16_t cx = x;
    uint8_t line = 0;

    while (*str && line < max_lines) {
        uint32_t cp = utf8_next(&str);
        if (cp == 0) break;

        if (cp == '\n') {
            line++;
            if (line >= max_lines) break;
            cx = x; y += line_h;
            continue;
        }

        int cw = (cp < 0x80) ? 8 * scale : 16 * scale;

        if (cx + cw > x + max_w) {
            line++;
            if (line >= max_lines) break;
            cx = x; y += line_h;
        }


        if (line == max_lines - 1 && *str && cx + cw + ell_w > x + max_w) {
            int eidx = font16_lookup(0x2026);
            if (eidx >= 0 && cx + ell_w <= x + max_w) {
                draw_cp(cx, y, 0x2026, color, bg, scale);
            }
            break;
        }

        cx += draw_cp(cx, y, cp, color, bg, scale);
    }
    return line + 1;
}


void button_init(void) {
    gpio_set_direction(BTN_A, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_A, GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_B, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_B, GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_C, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_C, GPIO_PULLUP_ONLY);
}
bool button_a_pressed(void) { return gpio_get_level(BTN_A) == 0; }
bool button_b_pressed(void) { return gpio_get_level(BTN_B) == 0; }
bool button_c_pressed(void) { return gpio_get_level(BTN_C) == 0; }
static void wait_button_release(void) {
    vTaskDelay(pdMS_TO_TICKS(20));
    while (button_a_pressed() || button_b_pressed() || button_c_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}


static void draw_exercise_name(uint16_t x, uint16_t y, exercise_t ex, uint16_t color, uint16_t bg) {
    const char *name = "躯干侧屈";
    if (ex == EXERCISE_SIDE_RAISE)      name = "侧平举";
    else if (ex == EXERCISE_ELBOW_CURL) name = "屈伸肘";
    tft_draw_utf8(x, y, name, color, bg, 2);
}


static int exercise_index = 0;
static const exercise_t exercise_list[] = {
    EXERCISE_EXPAND_CHEST,
    EXERCISE_SIDE_RAISE,
    EXERCISE_ELBOW_CURL
};
#define EXERCISE_LIST_COUNT   3


static int  last_total  = -1;
static int  last_standard = -1;
static int  last_nonstandard = -1;
static int  last_link = -1;
static int  last_hr     = -1;
static int  last_spo2   = -1;
static bool last_valid  = false;
static int  refresh_cnt = 0;


static void draw_welcome_screen(void) {
    tft_fill_screen(THEME_BG);


  draw_blueberry(10, 0, 2);
  draw_blueberry(TFT_WIDTH - 58, 0, 2);

    const char *line1 = "欢迎进入";
    const char *line2 = "智慧训练系统";
    tft_draw_utf8((480 - tft_utf8_width(line1, 2)) / 2,  50, line1, THEME_TEXT, THEME_BG, 2);
    tft_draw_utf8((480 - tft_utf8_width(line2, 2)) / 2, 110, line2, THEME_TEXT, THEME_BG, 2);

    const char *start = "开始";
    tft_draw_rect(190, 200, 100, 55, THEME_LINE);
    tft_draw_utf8(190 + (100 - tft_utf8_width(start, 2)) / 2, 208, start, THEME_TEXT, THEME_BG, 2);


    const char *b_hint = "B: AI助手";
    tft_fill_rect(310, 172, 160, 44, THEME_BG);
    tft_draw_rect(310, 172, 160, 44, THEME_LINE);
    tft_draw_utf8(310 + (160 - tft_utf8_width(b_hint, 2)) / 2, 180, b_hint, THEME_TEXT, THEME_BG, 2);
}


static void draw_select_screen(void) {
    tft_fill_screen(THEME_BG);


    draw_blueberry(10, 0, 2);
    draw_blueberry(TFT_WIDTH - 58, 0, 2);

    const char *title = "选择动作";
    tft_draw_utf8((480 - tft_utf8_width(title, 2)) / 2, 20, title, THEME_TEXT, THEME_BG, 2);

    const char *back = "返回";
    tft_draw_rect(10, 10, 90, 55, THEME_LINE);
    tft_draw_utf8(10 + (90 - tft_utf8_width(back, 2)) / 2, 22, back, THEME_TEXT, THEME_BG, 2);


    const char *btn1 = "躯干侧屈";
    tft_draw_rect(20, 100, 130, 55, THEME_LINE);
    tft_draw_utf8(20 + (130 - tft_utf8_width(btn1, 2)) / 2, 108, btn1, THEME_TEXT, THEME_BG, 2);

    const char *btn2 = "侧平举";
    tft_draw_rect(175, 100, 150, 55, THEME_LINE);
    tft_draw_utf8(175 + (150 - tft_utf8_width(btn2, 2)) / 2, 108, btn2, THEME_TEXT, THEME_BG, 2);

    const char *btn3 = "屈伸肘";
    tft_draw_rect(340, 100, 120, 55, THEME_LINE);
    tft_draw_utf8(340 + (120 - tft_utf8_width(btn3, 2)) / 2, 108, btn3, THEME_TEXT, THEME_BG, 2);

    if (exercise_index == 0) {
        tft_draw_rect(18, 98, 134, 59, THEME_HIGHLIGHT);
        tft_draw_rect(19, 99, 132, 57, THEME_HIGHLIGHT);
    } else if (exercise_index == 1) {
        tft_draw_rect(173, 98, 154, 59, THEME_HIGHLIGHT);
        tft_draw_rect(174, 99, 152, 57, THEME_HIGHLIGHT);
    } else if (exercise_index == 2) {
        tft_draw_rect(338, 98, 124, 59, THEME_HIGHLIGHT);
        tft_draw_rect(339, 99, 122, 57, THEME_HIGHLIGHT);
    }
}


static void update_monitor_data(void) {
    char buf[16];
    k230_status_t status;
    k230_get_status(&status);
    bool valid = g_watch_data_valid;
    int hr = g_watch_hr, spo2 = g_watch_spo2;
    int link = !status.connected ? 0 : !status.confirmed ? 1 : status.running ? 2 : 3;


    if (status.standard != last_standard) {
        tft_fill_rect(20, 105, 140, 48, THEME_BG);
        snprintf(buf, sizeof(buf), "%u", (unsigned)status.standard);
        tft_draw_string(20, 105, buf, THEME_TEXT, THEME_BG, 3);
        last_standard = status.standard;
    }
    if (status.nonstandard != last_nonstandard) {
        tft_fill_rect(180, 105, 140, 48, THEME_BG);
        snprintf(buf, sizeof(buf), "%u", (unsigned)status.nonstandard);
        tft_draw_string(180, 105, buf, THEME_TEXT, THEME_BG, 3);
        last_nonstandard = status.nonstandard;
    }
    if (status.total != last_total) {
        tft_fill_rect(340, 105, 120, 48, THEME_BG);
        snprintf(buf, sizeof(buf), "%u", (unsigned)status.total);
        tft_draw_string(340, 105, buf, THEME_TEXT, THEME_BG, 3);
        last_total = status.total;
    }
    if (hr != last_hr || valid != last_valid) {
        tft_fill_rect(90, 180, 130, 32, THEME_BG);
        if (valid) snprintf(buf, sizeof(buf), "%d bpm", hr);
        else snprintf(buf, sizeof(buf), "-- bpm");
        tft_draw_string(90, 180, buf, THEME_TEXT, THEME_BG, 2);
        last_hr = hr;
    }
    if (spo2 != last_spo2 || valid != last_valid) {
        tft_fill_rect(320, 180, 130, 32, THEME_BG);
        if (valid) snprintf(buf, sizeof(buf), "%d %%", spo2);
        else snprintf(buf, sizeof(buf), "-- %%");
        tft_draw_string(320, 180, buf, THEME_TEXT, THEME_BG, 2);
        last_spo2 = spo2;
    }
    last_valid = valid;
    if (link != last_link) {
        tft_fill_rect(20, 228, 440, 32, THEME_BG);
        const char *labels[] = {"K230 离线", "等待确认", "训练中", "已停止"};
        tft_draw_utf8(20, 228, labels[link], THEME_TEXT, THEME_BG, 2);
        last_link = link;
    }
}

static void draw_monitor_screen(void) {
    tft_fill_screen(THEME_BG);


    tft_draw_utf8(20, 6, "当前动作", THEME_TEXT, THEME_BG, 2);
    draw_exercise_name(148, 6, current_exercise, THEME_TEXT, THEME_BG);
    tft_draw_rect(0, 42, 480, 2, THEME_LINE);


    tft_draw_utf8(20, 65, "合格", THEME_TEXT, THEME_BG, 2);

    tft_draw_utf8(180, 65, "不合格", THEME_TEXT, THEME_BG, 2);

    tft_draw_utf8(340, 65, "总计", THEME_TEXT, THEME_BG, 2);

    tft_draw_rect(0, 155, 480, 2, THEME_LINE);


    tft_draw_utf8(20, 175, "心率", THEME_TEXT, THEME_BG, 2);

    tft_draw_utf8(250, 175, "血氧", THEME_TEXT, THEME_BG, 2);


    tft_draw_rect(380, 270, 90, 45, THEME_LINE);
    tft_draw_utf8(380 + (90 - tft_utf8_width("返回", 2)) / 2, 278, "返回", THEME_TEXT, THEME_BG, 2);
    last_total = last_standard = last_nonstandard = last_link = last_hr = last_spo2 = -1;
    update_monitor_data();
}


static void draw_ai_status(const char *status_text)
{
    tft_fill_rect(21, 71, 438, 148, THEME_BG);
    if (status_text && status_text[0])
        tft_draw_utf8_wrapped(40, 78, 400, 4, status_text, THEME_TEXT, THEME_BG, 2);
}

void draw_ai_chat_screen(const char *status_text)
{
    char snapshot[sizeof(s_ptt_status)];
    if (status_text == s_ptt_status) {
        taskENTER_CRITICAL(&s_ai_status_lock);
        memcpy(snapshot, s_ptt_status, sizeof(snapshot));
        taskEXIT_CRITICAL(&s_ai_status_lock);
        status_text = snapshot;
    }
    tft_fill_screen(THEME_BG);


    tft_draw_string(20, 10, "AI", THEME_TEXT, THEME_BG, 3);
    tft_draw_utf8(70, 14, "智慧助手", THEME_TEXT, THEME_BG, 2);

    tft_draw_rect(0, 50, 480, 2, THEME_LINE);


    tft_draw_rect(20, 70, 440, 150, THEME_LINE);
    draw_ai_status(status_text);


    const char *hint_a = "A: 开始";
    const char *hint_c = "C: 返回";
    tft_fill_rect(20, 245, 200, 42, THEME_BG);
    tft_draw_rect(20, 245, 200, 42, THEME_LINE);
    tft_draw_utf8(20 + (200 - tft_utf8_width(hint_a, 2)) / 2, 250, hint_a, THEME_TEXT, THEME_BG, 2);

    tft_fill_rect(240, 245, 220, 42, THEME_BG);
    tft_draw_rect(240, 245, 220, 42, THEME_LINE);
    tft_draw_utf8(240 + (220 - tft_utf8_width(hint_c, 2)) / 2, 250, hint_c, THEME_TEXT, THEME_BG, 2);


    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char mem_buf[48];
    snprintf(mem_buf, sizeof(mem_buf), "DRAM:%uK  PSRAM:%uK",
             (unsigned)(dram_free / 1024), (unsigned)(psram_free / 1024));
    tft_draw_string(10, 305, mem_buf, THEME_LINE, THEME_BG, 1);
}


static void count_number_text(unsigned value, char out[64])
{
    const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    const char *units[] = {"", "十", "百", "千", "万"};
    bool gap = false;
    out[0] = '\0';
    if (value == 0) { strcpy(out, digits[0]); return; }
    for (unsigned divisor = 10000, place = 4; divisor; divisor /= 10, place--) {
        unsigned digit = value / divisor;
        value %= divisor;
        if (!digit) {
            if (out[0] && value) gap = true;
            continue;
        }
        if (gap) { strcat(out, digits[0]); gap = false; }
        if (!(place == 1 && digit == 1 && !out[0])) strcat(out, digits[digit]);
        strcat(out, units[place]);
    }
}


static void sr_dispatch_cmd(voice_cmd_id_t cmd_id)
{
    ESP_LOGI(TAG, "Voice cmd #%d: %s", cmd_id, esp_sr_get_cmd_text(cmd_id));


    char tts_text[384] = {0};
    #define SR_TTS(text) do { \
        strncpy(tts_text, (text), sizeof(tts_text) - 1); \
        tts_text[sizeof(tts_text) - 1] = '\0'; \
    } while (0)

#if !VOICE_ALLOW_START_EXERCISE


    if (cmd_id == VOICE_CMD_JIN_XING_KUO_XIONG   ||
        cmd_id == VOICE_CMD_JIN_XING_FU_WO_CHENG ||
        cmd_id == VOICE_CMD_JIN_XING_SHEN_DUN    ||
        cmd_id == VOICE_CMD_KAI_SHI_JI_SHU) {
        ESP_LOGW(TAG, "Voice cmd #%d (%s) blocked — start exercise by pressing A",
                 cmd_id, esp_sr_get_cmd_text(cmd_id));
        local_tts_play("请按A键开始运动");
        return;
    }
#endif

    switch (cmd_id) {


    case VOICE_CMD_QING_JIN_RU_DOU_BAO:
        SR_TTS("好的，请按住A键说话");
        if (current_screen != SCREEN_AI_CHAT) {
            current_screen = SCREEN_AI_CHAT;
            draw_ai_chat_screen("按A开始AI对话");
        }
        break;


    case VOICE_CMD_FAN_HUI_SHANG_YI_JIE_MIAN:
        SR_TTS("好的，已经返回上一界面");
        if (current_screen == SCREEN_SELECT) {
            current_screen = SCREEN_WELCOME;
            draw_welcome_screen();
        }
        else if (current_screen == SCREEN_MONITOR) {
            k230_training_stop();
            current_screen = SCREEN_SELECT;
            draw_select_screen();
        }
        else if (current_screen == SCREEN_AI_CHAT) {
            current_screen = SCREEN_WELCOME;
            draw_welcome_screen();
        }
        break;


    case VOICE_CMD_JIN_XING_KUO_XIONG:
        if (current_screen == SCREEN_MONITOR) k230_training_stop();
        SR_TTS("好的，正在为您跳转躯干侧屈运动");
        current_exercise = EXERCISE_EXPAND_CHEST;
        exercise_index = 0;
        if (current_screen != SCREEN_SELECT) {
            current_screen = SCREEN_SELECT;
        }
        draw_select_screen();
        ESP_LOGI(TAG, "Voice: selected expand chest");
        break;


    case VOICE_CMD_JIN_XING_FU_WO_CHENG:
        if (current_screen == SCREEN_MONITOR) k230_training_stop();
        SR_TTS("好的，正在为您跳转侧平举运动");
        current_exercise = EXERCISE_SIDE_RAISE;
        exercise_index = 1;
        if (current_screen != SCREEN_SELECT) {
            current_screen = SCREEN_SELECT;
        }
        draw_select_screen();
        ESP_LOGI(TAG, "Voice: selected side raise");
        break;


    case VOICE_CMD_JIN_XING_SHEN_DUN:
        if (current_screen == SCREEN_MONITOR) k230_training_stop();
        SR_TTS("好的，正在为您跳转屈伸肘运动");
        current_exercise = EXERCISE_ELBOW_CURL;
        exercise_index = 2;
        if (current_screen != SCREEN_SELECT) {
            current_screen = SCREEN_SELECT;
        }
        draw_select_screen();
        ESP_LOGI(TAG, "Voice: selected elbow curl");
        break;


    case VOICE_CMD_KAI_SHI_JI_SHU:
        SR_TTS("好的");
        if (current_exercise != EXERCISE_NONE) {
            if (!k230_training_start(current_exercise - 1)) {
                SR_TTS("正在结束上一项运动，请稍后重试");
                break;
            }
            current_screen = SCREEN_MONITOR;
            last_total  = -1;
            last_hr     = -1;
            last_spo2   = -1;
            last_valid  = false;
            refresh_cnt = 0;
            draw_monitor_screen();
            ESP_LOGI(TAG, "Voice: entered MONITOR for exercise %d", current_exercise);
        } else {
            ESP_LOGW(TAG, "请开始计数 ignored: no exercise selected");
        }
        break;


    case VOICE_CMD_QING_JIE_SHU_DANG_QIAN_YUN_DONG: {
        SR_TTS("好的，正在退出当前运动");
        k230_training_stop();
        last_total  = -1;
        last_hr     = -1;
        last_spo2   = -1;
        current_exercise = EXERCISE_NONE;
        current_screen = SCREEN_WELCOME;
        draw_welcome_screen();
        ESP_LOGI(TAG, "Voice: training stopped; live counts cleared, awaiting next command");
        break;
    }


    case VOICE_CMD_BO_BAO_DANG_QIAN_JI_SHU: {
        k230_status_t status = {0};
        bool confirmed = k230_get_report(&status);
        if (status.session[0] && !confirmed) {
            SR_TTS("正在同步动作计数，请稍后再播报");
            ESP_LOGW(TAG, "COUNT_SPEECH waiting for K230 acknowledgement, session=%s", status.session);
            break;
        }
        int rate = status.total ? status.standard * 100 / status.total : 0;
        char total[64], standard[64], nonstandard[64], percent[64], buf[384];
        count_number_text(status.total, total);
        count_number_text(status.standard, standard);
        count_number_text(status.nonstandard, nonstandard);
        count_number_text(rate, percent);
        snprintf(buf, sizeof(buf), "%s完成%s次，合格%s次，不合格%s次，合格率百分之%s",
                 status.session[0] && !status.running ? "上次运动" : "当前",
                 total, standard, nonstandard, percent);
        SR_TTS(buf);
        ESP_LOGI(TAG, "COUNT_SPEECH total=%d standard=%d nonstandard=%d queued after redraw",
                 status.total, status.standard, status.nonstandard);
        break;
    }


    case VOICE_CMD_SHENG_CHENG_JIAN_SHEN_BAO_GAO: {
        if (s_ai_running || s_ptt_running) break;


        if (!is_wifi_connected()) {
            current_screen = SCREEN_AI_CHAT;
            draw_ai_chat_screen("未连接网络，无法生成报告");
            ai_status("未连接网络，无法生成报告");
            SR_TTS("未连接网络，请检查路由器");
            ESP_LOGW(TAG, "Report blocked: STA not connected to router");
            break;
        }
        k230_status_t report_status;
        if (!k230_get_report(&report_status)) {

            current_screen = SCREEN_AI_CHAT;
            draw_ai_chat_screen("请先完成训练并等待结束确认");
            ai_status("请先完成训练并等待结束确认");
            SR_TTS("请先完成训练并等待结束确认");
            break;
        }


        current_screen = SCREEN_AI_CHAT;
        draw_ai_chat_screen("正在生成健身报告…");
        ai_status("正在生成健身报告");
        s_report_job_status = report_status;
        s_ai_ok = false;
        s_ai_running = true;
        report_job_t job = REPORT_JOB_GENERATE;
        if (!s_report_job_q || xQueueSend(s_report_job_q, &job, 0) != pdTRUE) {
            s_ai_running = false;
            ai_status("报告任务繁忙，请稍后再试");
        }
        break;
    }


    case VOICE_CMD_JIN_RU_LIAO_TIAN_DUI_HUA:
        current_screen = SCREEN_AI_CHAT;
        if (!s_ai_running && !s_ptt_running) {
            ai_status("正在进入聊天对话…");
            draw_ai_chat_screen("正在进入聊天对话…");


            local_tts_play_wait("好的，已经进入AI聊天对话");
            vTaskDelay(pdMS_TO_TICKS(250));


            ai_status("请说话…");
            draw_ai_chat_screen("请说话…");
            xTaskCreate(ptt_task, "ptt", 16384, (void *)1, 3, NULL);
        } else {
            draw_ai_chat_screen("正在处理，请稍候…");
        }
        break;


    case VOICE_CMD_CHONG_SHU_JIAN_SHEN_BAO_GAO: {
        if (s_ai_running || s_ptt_running) {
            SR_TTS("正在处理，请稍候");
            break;
        }
        char *last = heap_caps_malloc(REPORT_CACHE_SIZE, MALLOC_CAP_SPIRAM);
        if (!last || !report_copy_last(last, REPORT_CACHE_SIZE)) {
            if (last) free(last);
            SR_TTS("还没有健身报告，请先说我生成健身报告");
            break;
        }
        current_screen = SCREEN_AI_CHAT;
        draw_ai_chat_screen("正在重述健身报告…");
        ai_status("正在重述健身报告");


        local_tts_play_wait("好的，正在重述中");
        s_report_job_text = last;
        s_ai_running = true;
        report_job_t job = REPORT_JOB_REPLAY;
        if (!s_report_job_q || xQueueSend(s_report_job_q, &job, 0) != pdTRUE) {
            free(last);
            s_report_job_text = NULL;
            s_ai_running = false;
            ai_status("重述任务繁忙，请稍后再试");
            local_tts_play("重述任务繁忙，请稍后再试");
        }
        break;
    }

    default:
        break;
    }


    if (tts_text[0]) local_tts_play(tts_text);
    #undef SR_TTS
}


void menu_init(void) {
    current_screen = SCREEN_WELCOME;
    exercise_count = 0;
    qualified_count = 0;
    unqualified_count = 0;
    exercise_active = false;
    current_exercise = EXERCISE_NONE;
    draw_welcome_screen();


    if (!s_report_worker) {
        if (!s_report_job_q) {
            s_report_job_q = xQueueCreate(2, sizeof(report_job_t));
        }
        if (s_report_job_q &&
            xTaskCreateWithCaps(report_worker_task, "rpt_work", 16384, NULL, 5,
                                &s_report_worker,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "Report worker create failed (PSRAM stack)");
            s_report_worker = NULL;
        }
        ESP_LOGI(TAG, "Report worker ready (PSRAM stack)");
    }
}

void menu_run(void) {


    if (s_redraw_pending) {
        s_redraw_pending = false;
        switch (current_screen) {
        case SCREEN_WELCOME:
            draw_welcome_screen();
            break;
        case SCREEN_SELECT:
            draw_select_screen();
            break;
        case SCREEN_MONITOR:
            last_total = last_standard = last_nonstandard = last_link = -1;
            last_hr = last_spo2 = -1;
            last_valid = false;
            draw_monitor_screen();
            break;
        case SCREEN_AI_CHAT: {
            char snap[sizeof(s_ptt_status)];
            taskENTER_CRITICAL(&s_ai_status_lock);
            memcpy(snap, s_ptt_status, sizeof(snap));
            taskEXIT_CRITICAL(&s_ai_status_lock);
            draw_ai_chat_screen(snap);
            break;
        }
        default:
            break;
        }
        return;
    }


    {
        int ui_cmd = s_k230_ui_cmd;
        if (ui_cmd >= 0) {
            int act = s_k230_ui_action;
            s_k230_ui_cmd = -1;
            if (ui_cmd == 0 && act >= 0 && act < EXERCISE_LIST_COUNT) {
                exercise_index = act;
                current_exercise = exercise_list[act];
                if (current_screen != SCREEN_MONITOR) {
                    current_screen = SCREEN_SELECT;
                    draw_select_screen();
                }
                ESP_LOGI(TAG, "K230 touch: select action %d", act);
                return;
            } else if (ui_cmd == 1 && act >= 0 && act < EXERCISE_LIST_COUNT) {
                bool started = k230_training_start(act);
                if (!started) {
                    local_tts_play("正在结束上一项运动，请稍后重试");
                } else {
                    exercise_index = act;
                    current_exercise = exercise_list[act];
                    current_screen = SCREEN_MONITOR;
                    last_total = last_standard = last_nonstandard = last_link = -1;
                    last_hr = last_spo2 = -1;
                    last_valid = false;
                    draw_monitor_screen();
                }
                ESP_LOGI(TAG, "K230 touch: start action %d (%s)", act, started ? "ok" : "rejected");
                return;
            } else if (ui_cmd == 2) {
                k230_training_stop();
                current_exercise = EXERCISE_NONE;
                if (current_screen == SCREEN_MONITOR) {
                    current_screen = SCREEN_SELECT;
                    draw_select_screen();
                }
                ESP_LOGI(TAG, "K230 touch: stop");
                return;
            } else if (ui_cmd == 3 && act >= 0 && act < EXERCISE_LIST_COUNT) {

                k230_status_t st;
                if (k230_get_status(&st) && st.running) {
                    ESP_LOGW(TAG, "Web: select ignored while running");
                    return;
                }
                exercise_index = act;
                current_exercise = exercise_list[act];
                if (current_screen != SCREEN_MONITOR) {
                    current_screen = SCREEN_SELECT;
                    draw_select_screen();
                }
                ESP_LOGI(TAG, "Web: select action %d", act);
                return;
            } else if (ui_cmd == 4 && act >= 0 && act < EXERCISE_LIST_COUNT) {

                exercise_index = act;
                current_exercise = exercise_list[act];
                current_screen = SCREEN_MONITOR;
                last_total = last_standard = last_nonstandard = last_link = -1;
                last_hr = last_spo2 = -1;
                last_valid = false;
                refresh_cnt = 0;
                draw_monitor_screen();
                ESP_LOGI(TAG, "Web: start action %d", act);
                return;
            } else if (ui_cmd == 5) {
                k230_status_t st;
                if (k230_get_status(&st) && st.running) {
                    ESP_LOGW(TAG, "Web: stop screen ignored, session still running");
                    return;
                }
                current_exercise = EXERCISE_NONE;
                if (current_screen == SCREEN_MONITOR) {
                    current_screen = SCREEN_SELECT;
                    draw_select_screen();
                }
                ESP_LOGI(TAG, "Web: stop, back to select");
                return;
            }
        }
    }


    if (current_screen == SCREEN_MONITOR) {
        k230_status_t status;
        if (k230_get_status(&status) && status.confirmed && !status.running && status.idle_timeout) {

            current_exercise = EXERCISE_NONE;
            current_screen = SCREEN_WELCOME;
            draw_welcome_screen();
            local_tts_play("长时间未检测到动作，已自动结束运动");
            ESP_LOGI(TAG, "K230 idle timeout: returned to welcome screen");
        }
        if (++refresh_cnt >= 3) {
            refresh_cnt = 0;
            if (current_screen == SCREEN_MONITOR) update_monitor_data();
        }
    } else {
        refresh_cnt = 0;
        last_total  = -1;
        last_hr     = -1;
        last_spo2   = -1;
        last_valid  = false;
    }

    bool status_changed;
    char status_snapshot[sizeof(s_ptt_status)];
    taskENTER_CRITICAL(&s_ai_status_lock);
    status_changed = s_ptt_status_dirty;
    s_ptt_status_dirty = false;
    if (status_changed) memcpy(status_snapshot, s_ptt_status, sizeof(status_snapshot));
    taskEXIT_CRITICAL(&s_ai_status_lock);
    if (status_changed && current_screen == SCREEN_AI_CHAT)
        draw_ai_status(status_snapshot);


    if (g_voice_cmd_queue) {
        voice_cmd_id_t vcmd;
        while (xQueueReceive(g_voice_cmd_queue, &vcmd, 0) == pdTRUE) {
            sr_dispatch_cmd(vcmd);
        }
    }

#if AUTO_SCROLL_DEMO

    {
        static int64_t s_scroll_next_us = 0;
        static int    s_scroll_step    = 0;
        int64_t now = esp_timer_get_time();
        if (s_scroll_next_us == 0) {
            s_scroll_next_us = now;
        }

        int64_t interval_us;
        if (s_scroll_step == 0)      interval_us = SCROLL_WELCOME_MS * 1000LL;
        else if (s_scroll_step == 1) interval_us = SCROLL_SELECT_MS  * 1000LL;
        else                         interval_us = SCROLL_MONITOR_MS * 1000LL;

        if (now - s_scroll_next_us >= interval_us) {
            s_scroll_next_us = now;
            s_scroll_step = (s_scroll_step + 1) % 3;

            switch (s_scroll_step) {
            case 0:
                current_screen = SCREEN_WELCOME;
                draw_welcome_screen();
                break;
            case 1:
                current_screen = SCREEN_SELECT;
                exercise_index = 0;
                draw_select_screen();
                break;
            default:
                current_exercise = exercise_list[exercise_index];
                current_screen = SCREEN_MONITOR;
                last_total  = -1;
                last_hr     = -1;
                last_spo2   = -1;
                last_valid  = false;
                refresh_cnt = 0;
                draw_monitor_screen();
                break;
            }
        }
    }
#else

    static int64_t s_last_button_time_us = 0;
    int64_t now_us = esp_timer_get_time();
    bool in_cooldown = (s_last_button_time_us > 0 &&
                        (now_us - s_last_button_time_us) < 400000);

    if (!in_cooldown && button_a_pressed()) {
        s_last_button_time_us = now_us;
        if (current_screen == SCREEN_AI_CHAT) {

            if (!s_ai_running && !s_ptt_running)
                xTaskCreate(ptt_task, "ptt", 16384, NULL, 3, NULL);
        } else {
            wait_button_release();
            if (current_screen == SCREEN_WELCOME) {
                current_screen = SCREEN_SELECT;
                exercise_index = 0;
                draw_select_screen();
            }
            else if (current_screen == SCREEN_SELECT) {
                current_exercise = exercise_list[exercise_index];

                if (!k230_training_start(current_exercise - 1)) {
                    local_tts_play("正在结束上一项运动，请稍后重试");
                    return;
                }

                current_screen = SCREEN_MONITOR;
                last_total  = -1;
                last_hr     = -1;
                last_spo2   = -1;
                last_valid  = false;
                refresh_cnt = 0;
                draw_monitor_screen();
            }
        }
    }

    if (!in_cooldown && button_b_pressed()) {
        s_last_button_time_us = now_us;
        wait_button_release();
        if (current_screen == SCREEN_WELCOME) {

            current_screen = SCREEN_AI_CHAT;
            draw_ai_chat_screen("按A开始AI对话");
            ESP_LOGI(TAG, "Entered AI Chat screen");
        }
        else if (current_screen == SCREEN_SELECT) {
            exercise_index = (exercise_index + 1) % EXERCISE_LIST_COUNT;
            draw_select_screen();
            ESP_LOGI(TAG, "Selected exercise: %d", exercise_index);
        }
    }

    if (!in_cooldown && button_c_pressed()) {
        s_last_button_time_us = now_us;
        wait_button_release();
        if (current_screen == SCREEN_SELECT) {
            current_screen = SCREEN_WELCOME;
            draw_welcome_screen();
        }
        else if (current_screen == SCREEN_MONITOR) {
            k230_training_stop();
            current_screen = SCREEN_SELECT;
            draw_select_screen();
        }
        else if (current_screen == SCREEN_AI_CHAT) {
            current_screen = SCREEN_WELCOME;
            draw_welcome_screen();
        }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(15));
}
