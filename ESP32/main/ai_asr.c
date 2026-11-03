#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "mbedtls/base64.h"
#include "ai_asr.h"
#include "ai_chat.h"

static const char *TAG = "AI_ASR";
static volatile ai_asr_status_t s_last_status = AI_ASR_STATUS_FAIL;

static int64_t s_busy_until_us = 0;
ai_asr_status_t ai_asr_last_status(void) { return s_last_status; }


#define VOLC_ASR_URL  "https://openspeech.bytedance.com/api/v1/asr"


static char *json_extract_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);

    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return NULL;
    p++;

    const char *end = strchr(p, '"');
    if (!end) return NULL;

    size_t len = end - p;
    char *result = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!result) return NULL;
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}


static char *http_read_response(esp_http_client_handle_t client, size_t *out_len)
{
    int content_length = esp_http_client_get_content_length(client);
    size_t buf_size = (content_length > 0) ? (size_t)content_length + 1 : 4096;
    size_t capacity = buf_size;
    char *buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating HTTP response buffer");
        return NULL;
    }

    size_t total = 0;
    int read_len;
    while (1) {
        if (total + 512 > capacity) {
            capacity *= 2;
            char *new_buf = heap_caps_realloc(buf, capacity, MALLOC_CAP_SPIRAM);
            if (!new_buf) { free(buf); return NULL; }
            buf = new_buf;
        }
        read_len = esp_http_client_read(client, buf + total, capacity - total - 1);
        if (read_len <= 0) break;
        total += read_len;
    }
    if (total > 0) buf[total] = '\0';
    else buf[0] = '\0';
    if (out_len) *out_len = total;
    return buf;
}


char *ai_asr_recognize(const int16_t *pcm, size_t samples)
{
    s_last_status = AI_ASR_STATUS_FAIL;
    if (s_busy_until_us && esp_timer_get_time() < s_busy_until_us) {
        s_last_status = AI_ASR_STATUS_BUSY;
        ESP_LOGW(TAG, "ASR cooldown after quota error");
        return NULL;
    }
    if (!pcm || samples == 0) {
        ESP_LOGW(TAG, "Empty PCM data");
        return NULL;
    }

    size_t pcm_bytes = samples * sizeof(int16_t);
    ESP_LOGI(TAG, "ASR: %u samples (%.1f s), encoding to base64...",
             (unsigned)samples, (double)samples / 16000.0);


    size_t b64_max = (pcm_bytes * 4 + 2) / 3 + 4;
    char *b64 = heap_caps_malloc(b64_max, MALLOC_CAP_SPIRAM);
    if (!b64) {
        ESP_LOGE(TAG, "OOM allocating base64 buffer (%u bytes)", (unsigned)b64_max);
        return NULL;
    }

    size_t b64_len = 0;
    int ret = mbedtls_base64_encode((unsigned char *)b64, b64_max, &b64_len,
                                    (const unsigned char *)pcm, pcm_bytes);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", ret);
        free(b64);
        return NULL;
    }
    ESP_LOGI(TAG, "Base64: %u bytes PCM → %u chars base64", (unsigned)pcm_bytes, (unsigned)b64_len);


    size_t body_size = b64_len + 1024;
    char *body = heap_caps_malloc(body_size, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "OOM allocating JSON body");
        free(b64);
        return NULL;
    }

    snprintf(body, body_size,
             "{"
             "\"app\":{"
               "\"appid\":\"%s\","
               "\"token\":\"%s\","
               "\"cluster\":\"volcengine_input_common\""
             "},"
             "\"user\":{\"uid\":\"esp32_guidance\"},"
             "\"audio\":{"
               "\"format\":\"pcm\","
               "\"rate\":16000,"
               "\"bits\":16,"
               "\"channel\":1,"
               "\"language\":\"zh-CN\","
               "\"data\":\"%s\""
             "},"
             "\"request\":{"
               "\"reqid\":\"esp32_%lu\","
               "\"sequence\":1,"
               "\"nbest\":1"
             "}"
             "}",
             VOLC_TTS_APP_ID, VOLC_TTS_TOKEN,
             b64,
             (unsigned long)esp_timer_get_time());
    free(b64);

    ESP_LOGI(TAG, "ASR JSON body: %u bytes", (unsigned)strlen(body));


    esp_http_client_config_t config = {
        .url = VOLC_ASR_URL,
        .timeout_ms = 30000,
        .buffer_size = 2048,


        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(body);
        return NULL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer;%s", VOLC_TTS_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);

    int body_len = (int)strlen(body);


    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(body);
        return NULL;
    }


    int wlen = esp_http_client_write(client, body, body_len);
    free(body);
    if (wlen != body_len) {
        ESP_LOGE(TAG, "HTTP write failed: %d/%d", wlen, body_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }


    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "ASR HTTP %d, Content-Length=%lld", status, (long long)content_len);

    if (status != 200) {
        s_last_status = (status == 429) ? AI_ASR_STATUS_BUSY : AI_ASR_STATUS_FAIL;


        if (status == 429) s_busy_until_us = esp_timer_get_time() + 20000000;
        char *err_body = http_read_response(client, NULL);
        if (err_body && err_body[0]) {
            ESP_LOGE(TAG, "ASR error body: %s", err_body);
        }
        free(err_body);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }


    size_t resp_len = 0;
    char *resp = http_read_response(client, &resp_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!resp || resp_len == 0) {
        ESP_LOGE(TAG, "ASR empty response");
        free(resp);
        return NULL;
    }

    ESP_LOGI(TAG, "ASR response (len=%u): %.300s", (unsigned)resp_len, resp);


    char *text = json_extract_string(resp, "text");

    if (!text) {

        char *code_str = json_extract_string(resp, "code");
        if (code_str) {
            ESP_LOGI(TAG, "ASR code: %s", code_str);
            free(code_str);
        }

        text = heap_caps_malloc(1, MALLOC_CAP_SPIRAM);
        if (text) text[0] = '\0';
    }

    free(resp);

    if (text && text[0]) {
        s_last_status = AI_ASR_STATUS_OK;
        ESP_LOGI(TAG, "ASR recognized: \"%s\"", text);
    } else {
        s_last_status = AI_ASR_STATUS_NO_SPEECH;
        ESP_LOGW(TAG, "ASR: no speech recognized (empty result)");
    }

    return text;
}
