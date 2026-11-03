#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "ai_chat.h"
#include "i2s_audio.h"

static const char *TAG = "AI_CHAT";

static const char *chat_failure_message(const char *json)
{
    const char *message = "报告生成失败，请查看串口HTTP/API错误";
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    if (cJSON_IsString(code) && !strcmp(code->valuestring, "InvalidEndpoint.ClosedEndpoint")) {
        message = "AI接入点已关闭或暂不可用，请在火山方舟启用或更换接入点";
        ESP_LOGE(TAG, "Chat endpoint %s is closed/unavailable; enable it or update DOUBAO_ENDPOINT_ID", DOUBAO_ENDPOINT_ID);
    }
    cJSON_Delete(root);
    return message;
}


static void trim_to_last_sentence(char *text)
{
    size_t n = strlen(text), cut = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            if (c == '.' || c == '!' || c == '?' || c == '\n') cut = i + 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < n &&
                ((c == 0xE3 && (unsigned char)text[i+1] == 0x80 &&
                  (unsigned char)text[i+2] == 0x82) ||
                 (c == 0xEF && (unsigned char)text[i+1] == 0xBC &&
                  ((unsigned char)text[i+2] == 0x81 ||
                   (unsigned char)text[i+2] == 0x9F ||
                   (unsigned char)text[i+2] == 0x9B))))
                cut = i + 3;
            i += 3;
        } else {
            i += 4;
        }
    }
    if (cut > 0 && cut < n) {
        text[cut] = '\0';
        ESP_LOGW(TAG, "Reply trimmed to %u bytes at last complete sentence", (unsigned)cut);
    }
}


static char *parse_chat_reply(const char *json, bool *out_truncated)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(choice0, "message");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice0, "finish_reason");
    if (out_truncated)
        *out_truncated = cJSON_IsString(finish) && !strcmp(finish->valuestring, "length");
    char *reply = NULL;
    if (cJSON_IsString(content) && content->valuestring[0]) reply = strdup(content->valuestring);
    else ESP_LOGE(TAG, "Chat response has no choices[0].message.content");
    cJSON_Delete(root);
    return reply;
}

static char *build_chat_body_ex(const char *message, bool stream)
{
    cJSON *root = cJSON_Parse(stream ?
        "{\"model\":\"\",\"messages\":[{\"role\":\"system\",\"content\":\"\"},"
        "{\"role\":\"user\",\"content\":\"\"}],\"max_tokens\":220,\"temperature\":0.7,"
        "\"thinking\":{\"type\":\"disabled\"},\"stream\":true}" :
        "{\"model\":\"\",\"messages\":[{\"role\":\"system\",\"content\":\"\"},"


        "{\"role\":\"user\",\"content\":\"\"}],\"max_tokens\":400,\"temperature\":0.7,"
        "\"thinking\":{\"type\":\"disabled\"},\"stream\":false}");
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    char *body = NULL;
    if (root && cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(root, "model"), DOUBAO_ENDPOINT_ID) &&
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "content"), AI_SYSTEM_PROMPT) &&
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 1), "content"), message))
        body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static char *build_chat_body(const char *message)
{
    return build_chat_body_ex(message, false);
}

static char *build_tts_body(const char *text)
{
    cJSON *root = cJSON_Parse("{\"app\":{\"appid\":\"\",\"token\":\"\",\"cluster\":\"volcano_tts\"},"
                             "\"user\":{\"uid\":\"esp32_guidance\"},"
                             "\"audio\":{\"voice_type\":\"BV001_streaming\",\"encoding\":\"pcm\","
                             "\"sample_rate\":16000,\"bits_per_sample\":16,\"channel\":1,"
                             "\"speed_ratio\":1.2,\"volume_ratio\":1.0},"
                             "\"request\":{\"reqid\":\"\",\"operation\":\"query\",\"text\":\"\"}}");
    cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");
    cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "request");
    uint8_t id[16];
    esp_fill_random(id, sizeof(id));
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    char reqid[37];
    snprintf(reqid, sizeof(reqid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    char *body = NULL;
    if (root && cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(app, "appid"), VOLC_TTS_APP_ID) &&
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(app, "token"), VOLC_TTS_TOKEN) &&
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(request, "reqid"), reqid) &&
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(request, "text"), text))
        body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}


static char *http_read_response(esp_http_client_handle_t client, size_t *out_len)
{
    int content_length = esp_http_client_get_content_length(client);
    if (content_length > 4 * 1024 * 1024) return NULL;
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
        if (content_length > 0 && total == (size_t)content_length) break;
        if (total + 1 >= capacity) {
            size_t next = capacity * 2;
            if (next > 4 * 1024 * 1024 + 1) next = 4 * 1024 * 1024 + 1;
            if (next <= capacity) { free(buf); return NULL; }
            char *new_buf = heap_caps_realloc(buf, next, MALLOC_CAP_SPIRAM);
            if (!new_buf) {
                ESP_LOGE(TAG, "OOM reallocating HTTP buffer");
                free(buf);
                return NULL;
            }
            buf = new_buf;
            capacity = next;
        }
        read_len = esp_http_client_read(client, buf + total, capacity - total - 1);
        if (read_len < 0) { free(buf); return NULL; }
        if (read_len == 0) break;
        total += read_len;
    }
    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG, "HTTP response incomplete (%u bytes)", (unsigned)total);
        free(buf);
        return NULL;
    }
    if (total > 0) {
        buf[total] = '\0';
    } else {
        buf[0] = '\0';
    }
    if (out_len) *out_len = total;
    ESP_LOGI(TAG, "http_read_response: read %u bytes total", (unsigned)total);
    return buf;
}


static uint8_t *http_read_binary_response(esp_http_client_handle_t client, size_t *out_len)
{
    int content_length = esp_http_client_get_content_length(client);

    size_t capacity = (content_length > 0) ? (size_t)content_length : 65536;
    uint8_t *buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating binary buffer");
        return NULL;
    }

    size_t total = 0;
    int read_len;
    while (1) {
        if (total + 1024 > capacity) {
            capacity += 32768;
            uint8_t *new_buf = heap_caps_realloc(buf, capacity, MALLOC_CAP_SPIRAM);
            if (!new_buf) {
                ESP_LOGE(TAG, "OOM reallocating binary buffer at %u bytes", (unsigned)total);
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        read_len = esp_http_client_read(client, (char *)(buf + total), capacity - total);
        if (read_len <= 0) break;
        total += read_len;
    }
    ESP_LOGI(TAG, "Downloaded %u bytes of audio data", (unsigned)total);
    if (out_len) *out_len = total;
    return buf;
}


static esp_err_t http_post_json(const char *url, const char *auth_header,
                                const char *json_body, char **out_response, size_t *out_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 45000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    if (out_response) *out_response = NULL;
    if (out_len) *out_len = 0;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }


    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");

    int body_len = (int)strlen(json_body);
    ESP_LOGI(TAG, "POST %s (body %d bytes)", url, body_len);


    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }


    for (int sent = 0; sent < body_len;) {
        int n = esp_http_client_write(client, json_body + sent, body_len - sent);
        if (n <= 0) {
            ESP_LOGE(TAG, "HTTP write failed at %d/%d", sent, body_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        sent += n;
    }


    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, Content-Length=%lld", status, (long long)content_length);

    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }


    if (out_response && status >= 200 && status < 300) {
        *out_response = http_read_response(client, out_len);
        if (!*out_response) err = ESP_FAIL;
    } else if (status >= 400) {

        char *err_body = http_read_response(client, NULL);
        if (err_body && err_body[0]) {
            ESP_LOGE(TAG, "HTTP %d error body: %.512s", status, err_body);
        }

        if (out_response) *out_response = err_body;
        else free(err_body);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (status >= 200 && status < 300) ? err : ESP_FAIL;
}


static char *ai_chat_send_with_error(const char *user_message, const char **out_error)
{
    if (out_error) *out_error = "报告生成失败，请查看串口HTTP/API错误";
    if (!user_message || !user_message[0]) return NULL;
    char *body = build_chat_body(user_message);
    if (!body) return NULL;
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", DOUBAO_API_KEY);
    char *response = NULL;
    size_t resp_len = 0;
    esp_err_t err = http_post_json(DOUBAO_CHAT_URL, auth, body, &response, &resp_len);
    free(body);
    if (err != ESP_OK || !response) {
        const char *message = chat_failure_message(response);
        if (out_error) *out_error = message;
        ESP_LOGE(TAG, "Chat API call failed");
        free(response);
        return NULL;
    }
    bool truncated = false;
    char *reply = parse_chat_reply(response, &truncated);
    if (reply) {


        size_t len = strlen(reply);
        if (len > 600) {
            size_t n = 600;
            while (n > 0 && ((unsigned char)reply[n] & 0xC0) == 0x80) n--;
            reply[n] = '\0';
            truncated = true;
            ESP_LOGW(TAG, "Chat reply capped to %u bytes (was %u)", (unsigned)n, (unsigned)len);
        }
        if (truncated) {
            ESP_LOGW(TAG, "Chat reply was cut; trimming to a complete sentence");
            trim_to_last_sentence(reply);
        }
    }
    if (reply) ESP_LOGI(TAG, "AI reply (%u bytes): %s", (unsigned)strlen(reply), reply);
    free(response);
    if (reply && out_error) *out_error = NULL;
    return reply;
}

char *ai_chat_send(const char *user_message)
{
    return ai_chat_send_with_error(user_message, NULL);
}


static bool sse_ends_sentence(const char *buf, size_t n)
{
    if (!n) return false;
    char c = buf[n - 1];
    if (c == '\n' || c == '.' || c == '!' || c == '?') return true;


    if (n >= 3 && (unsigned char)buf[n-3] == 0xE3 &&
        (unsigned char)buf[n-2] == 0x80 && (unsigned char)buf[n-1] == 0x82) return true;
    if (n >= 3 && (unsigned char)buf[n-3] == 0xEF && (unsigned char)buf[n-2] == 0xBC &&
        (buf[n-1] == (char)0x81 || buf[n-1] == (char)0x9F ||
         buf[n-1] == (char)0x9B)) return true;
    return false;
}

char *ai_chat_stream(const char *user_message, ai_sentence_cb_t on_sentence, void *user)
{
    if (!user_message || !user_message[0]) return NULL;

    char *body = build_chat_body_ex(user_message, true);
    if (!body) return NULL;

    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", DOUBAO_API_KEY);

    esp_http_client_config_t config = {
        .url = DOUBAO_CHAT_URL,
        .timeout_ms = 60000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(body); return NULL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    int body_len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream: HTTP open failed: %s", esp_err_to_name(err));
        free(body); esp_http_client_cleanup(client); return NULL;
    }
    for (int sent = 0; sent < body_len;) {
        int n = esp_http_client_write(client, body + sent, body_len - sent);
        if (n <= 0) {
            ESP_LOGE(TAG, "stream: HTTP write failed at %d/%d", sent, body_len);
            free(body); esp_http_client_close(client); esp_http_client_cleanup(client); return NULL;
        }
        sent += n;
    }
    free(body);

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "stream: HTTP %d, Content-Length=%lld", status, (long long)content_length);
    if (status < 200 || status >= 300) {
        char *err_body = http_read_response(client, NULL);
        if (err_body && err_body[0]) ESP_LOGE(TAG, "stream HTTP %d: %.512s", status, err_body);
        free(err_body);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }


    const size_t sse_line_max = 2048;
    const size_t sse_text_max = 4096;
    char *line = heap_caps_malloc(sse_line_max, MALLOC_CAP_SPIRAM);
    char *reply = heap_caps_malloc(sse_text_max, MALLOC_CAP_SPIRAM);
    char sentence[512];
    if (!line || !reply) {
        free(line); free(reply);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return NULL;
    }
    size_t line_len = 0, reply_len = 0, sent_len = 0;
    reply[0] = '\0';

    char rbuf[1024];
    bool done = false;
    int idle = 0;
    int64_t t_start = esp_timer_get_time();
    int64_t t_first = 0;
    int sse_dbg = 0;

    while (!done) {
        int n = esp_http_client_read(client, rbuf, sizeof(rbuf));
        if (n < 0) break;
        if (n == 0) {
            if (++idle >= 100) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        idle = 0;
        for (int i = 0; i < n && !done; i++) {
            char c = rbuf[i];
            if (c != '\n') {
                if (line_len < sse_line_max - 1) line[line_len++] = c;
                continue;
            }
            line[line_len] = '\0';
            if (line_len && line[line_len - 1] == '\r') line[--line_len] = '\0';
            line_len = 0;

            if (strncmp(line, "data:", 5) != 0) continue;
            const char *payload = line + 5;
            while (*payload == ' ') payload++;
            if (!strcmp(payload, "[DONE]")) { done = true; break; }

            cJSON *j = cJSON_Parse(payload);
            if (!j) continue;
            cJSON *choices = cJSON_GetObjectItemCaseSensitive(j, "choices");
            cJSON *delta = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "delta");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");

            if (sse_dbg < 3) {
                ESP_LOGI(TAG, "SSE[%d]: %.400s", sse_dbg, payload);
                sse_dbg++;
            }
            if (cJSON_IsString(content) && content->valuestring[0]) {
                const char *s = content->valuestring;
                size_t sl = strlen(s);
                if (!t_first) {
                    t_first = esp_timer_get_time();
                    ESP_LOGI(TAG, "stream: first token after %.2fs", (double)(t_first - t_start) / 1e6);
                }
                if (reply_len + sl < sse_text_max - 1) {
                    memcpy(reply + reply_len, s, sl);
                    reply_len += sl;
                    reply[reply_len] = '\0';
                }
                for (size_t k = 0; k < sl; k++) {
                    if (sent_len >= sizeof(sentence) - 1) {

                        sentence[sent_len] = '\0';
                        if (on_sentence) on_sentence(sentence, user);
                        sent_len = 0;
                    }
                    sentence[sent_len++] = s[k];
                    if (sent_len >= 4 && sse_ends_sentence(sentence, sent_len)) {
                        sentence[sent_len] = '\0';
                        if (on_sentence) on_sentence(sentence, user);
                        sent_len = 0;
                    }
                }
            }
            cJSON_Delete(j);
        }
    }

    if (sent_len) {
        sentence[sent_len] = '\0';
        if (on_sentence) on_sentence(sentence, user);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(line);

    ESP_LOGI(TAG, "stream: reply %u bytes in %.2fs", (unsigned)reply_len,
             (double)(esp_timer_get_time() - t_start) / 1e6);
    if (reply_len == 0) { free(reply); return NULL; }
    return reply;
}


static int16_t *parse_tts_audio(const char *json, size_t *out_samples)
{
    *out_samples = 0;
    cJSON *root = cJSON_Parse(json);
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 3000 ||
        !cJSON_IsString(data) || !data->valuestring[0]) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGE(TAG, "TTS API rejected: code=%d message=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_IsString(message) ? message->valuestring : "invalid JSON/audio");
        cJSON_Delete(root);
        return NULL;
    }
    size_t b64_len = strlen(data->valuestring);
    size_t capacity = b64_len * 3 / 4 + 4, pcm_bytes = 0;
    uint8_t *pcm = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    int ret = pcm ? mbedtls_base64_decode(pcm, capacity, &pcm_bytes,
                      (const unsigned char *)data->valuestring, b64_len) : -1;
    cJSON_Delete(root);
    if (ret != 0 || pcm_bytes < 2 || pcm_bytes % 2) {
        ESP_LOGE(TAG, "TTS invalid PCM/base64: ret=%d bytes=%u", ret, (unsigned)pcm_bytes);
        free(pcm);
        return NULL;
    }
    *out_samples = pcm_bytes / 2;
    return (int16_t *)pcm;
}

int16_t *ai_tts_synthesize(const char *text, size_t *out_samples)
{
    if (!out_samples) return NULL;
    *out_samples = 0;
    if (!text || !text[0]) return NULL;
    char *body = build_tts_body(text);
    if (!body) return NULL;
    char auth[160];

    snprintf(auth, sizeof(auth), "Bearer;%s", VOLC_TTS_TOKEN);
    char *response = NULL;
    esp_err_t err = http_post_json(VOLC_TTS_URL, auth, body, &response, NULL);
    free(body);
    int16_t *audio = err == ESP_OK && response ? parse_tts_audio(response, out_samples) : NULL;
    free(response);
    if (!audio) return NULL;
    uint8_t *pcm = (uint8_t *)audio;
    size_t pcm_bytes = *out_samples * sizeof(int16_t);

    #define TTS_GAIN_NUM  1
    #define TTS_GAIN_DEN  1
    {
        size_t n = pcm_bytes / 2;
        int16_t *s = (int16_t *)pcm;
        int32_t peak_before = 0, peak_after = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)s[i] * TTS_GAIN_NUM / TTS_GAIN_DEN;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            int32_t abs_before = s[i] >= 0 ? s[i] : -s[i];
            if (abs_before > peak_before) peak_before = abs_before;
            int32_t abs_after = v >= 0 ? v : -v;
            if (abs_after > peak_after) peak_after = abs_after;
            s[i] = (int16_t)v;
            if ((i & 0x7FF) == 0) vTaskDelay(1);
        }
        ESP_LOGI(TAG, "TTS gain: %dx, peak %ld → %ld",
                 TTS_GAIN_NUM, (long)peak_before, (long)peak_after);
    }
    #undef TTS_GAIN_NUM
    #undef TTS_GAIN_DEN


    ESP_LOGI(TAG, "TTS final: %u PCM samples", (unsigned)*out_samples);
    return audio;
}


esp_err_t ai_conversation(const char *question)
{
    ESP_LOGI(TAG, "=== AI Conversation Start ===");
    ESP_LOGI(TAG, "Question: %s", question);


    char *reply = ai_chat_send(question);
    if (!reply) {
        ESP_LOGE(TAG, "Chat failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Step 1 done, reply=%s", reply);


    size_t reply_len = strlen(reply);
    if (reply_len > 200) {
        for (size_t i = 200; i > 100; i--) {
            if (reply[i] == '\n') { reply[i] = '\0'; break; }
        }
        ESP_LOGI(TAG, "Reply truncated: %u → %u chars", (unsigned)reply_len, (unsigned)strlen(reply));
    }

    vTaskDelay(pdMS_TO_TICKS(100));


    ESP_LOGI(TAG, "Step 2: calling TTS...");
    size_t audio_samples = 0;
    int16_t *pcm = ai_tts_synthesize(reply, &audio_samples);
    if (!pcm || audio_samples == 0) {
        ESP_LOGE(TAG, "TTS failed");
        free(reply);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Step 2 done, %u samples", (unsigned)audio_samples);


    ESP_LOGI(TAG, "Step 3: playing audio...");
    esp_err_t ret = i2s_audio_play(pcm, audio_samples);

    free(pcm);
    free(reply);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "=== AI Conversation Done ===");
    }
    return ret;
}


char *ai_chat_text_only(const char *user_message)
{
    return ai_chat_send(user_message);
}

char *ai_chat_text_only_with_error(const char *user_message, const char **out_error)
{
    return ai_chat_send_with_error(user_message, out_error);
}
