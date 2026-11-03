#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "espnow_comm.h"
#include "uart_startup.h"
#include "k230_tcp.h"
#include "tft_spi.h"
#include "web_server.h"
#include "wifi.h"
#include "local_tts.h"

static const char *TAG = "WEB";


extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");


#define HIST_MAX              20
#define HIST_ERR_COUNT        10
#define HIST_STORE_MAGIC      0x57454248u
#define HIST_STORE_VERSION    1
#define HIST_NVS_NAMESPACE    "webhist"
#define HIST_NVS_KEY          "records"

typedef struct __attribute__((packed)) {
    char     date_str[12];
    char     time_str[12];
    char     session[24];
    int32_t  action;
    int32_t  total;
    int32_t  standard;
    int32_t  nonstandard;
    int32_t  incomplete;
    int32_t  interrupted;
    int32_t  short_hold;
    int32_t  score;
    int32_t  score_sum;
    int32_t  hr_avg;
    int32_t  hr_min;
    int32_t  hr_max;
    int32_t  spo2_avg;
    int32_t  spo2_min;
    uint32_t duration_s;
    int32_t  errors[HIST_ERR_COUNT];
} history_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t head;
    uint16_t reserved;
    history_entry_t entries[HIST_MAX];
} history_store_t;

static history_store_t s_store = {
    .magic = HIST_STORE_MAGIC,
    .version = HIST_STORE_VERSION,
};
static SemaphoreHandle_t s_history_mutex = NULL;
static char s_recorded_session[24];


static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static bool     s_sample_active = false;
static char     s_sample_session[24];
static int64_t  s_sample_start_us = 0;
static uint32_t s_sample_count = 0;
static uint32_t s_sample_hr_sum = 0;
static uint32_t s_sample_spo2_sum = 0;
static int      s_sample_hr_min = 0;
static int      s_sample_hr_max = 0;
static int      s_sample_spo2_min = 0;


#define HR_ALERT_LOW             80
#define HR_ALERT_HIGH            110
#define SPO2_ALERT_LOW           90
#define ALERT_CONFIRM_SAMPLES    3
#define ALERT_COOLDOWN_SEC       30
#define ALERT_START_GRACE_SEC    10

typedef enum {
    PHYS_ZONE_NORMAL = 0,
    PHYS_ZONE_HR_LOW,
    PHYS_ZONE_HR_HIGH,
    PHYS_ZONE_SPO2_LOW,
} phys_zone_t;

static phys_zone_t s_alerted_zone    = PHYS_ZONE_NORMAL;
static phys_zone_t s_candidate_zone  = PHYS_ZONE_NORMAL;
static int         s_candidate_count = 0;
static int64_t     s_last_alert_us   = 0;
static int64_t     s_first_action_us = 0;

static void phys_alert_reset(void)
{
    s_alerted_zone    = PHYS_ZONE_NORMAL;
    s_candidate_zone  = PHYS_ZONE_NORMAL;
    s_candidate_count = 0;
}

static void phys_alert_evaluate(bool running, int hr, int spo2, bool valid)
{
    if (!running) {
        phys_alert_reset();
        return;
    }
    if (!valid || hr <= 0 || spo2 <= 0) {
        s_candidate_zone  = PHYS_ZONE_NORMAL;
        s_candidate_count = 0;
        return;
    }

    int64_t now = esp_timer_get_time();

    phys_zone_t zone = PHYS_ZONE_NORMAL;
    if (hr < HR_ALERT_LOW) {
        zone = PHYS_ZONE_HR_LOW;
    } else if (hr > HR_ALERT_HIGH) {
        zone = PHYS_ZONE_HR_HIGH;
    } else if (spo2 < SPO2_ALERT_LOW) {
        zone = PHYS_ZONE_SPO2_LOW;
    }

    if (zone == PHYS_ZONE_NORMAL) {

        phys_alert_reset();
        return;
    }
    if (zone == s_alerted_zone) {

        s_candidate_zone  = zone;
        s_candidate_count = 0;
        return;
    }
    if (zone != s_candidate_zone) {
        s_candidate_zone  = zone;
        s_candidate_count = 0;
    }
    if (++s_candidate_count < ALERT_CONFIRM_SAMPLES) {
        return;
    }

    if (s_last_alert_us != 0 &&
        (now - s_last_alert_us) < (int64_t)ALERT_COOLDOWN_SEC * 1000000LL) {
        return;
    }

    s_alerted_zone    = zone;
    s_candidate_zone  = PHYS_ZONE_NORMAL;
    s_candidate_count = 0;
    s_last_alert_us   = now;

    switch (zone) {
        case PHYS_ZONE_HR_LOW:
            ESP_LOGW(TAG, "ALERT low intensity: HR=%d < %d", hr, HR_ALERT_LOW);

            break;
        case PHYS_ZONE_HR_HIGH:
            ESP_LOGW(TAG, "ALERT high intensity: HR=%d > %d", hr, HR_ALERT_HIGH);
            local_tts_play("您当前训练强度过高，请减轻运动幅度避免过度劳累或受伤哦。");
            break;
        case PHYS_ZONE_SPO2_LOW:
            ESP_LOGW(TAG, "ALERT low SpO2: %d%% < %d%%", spo2, SPO2_ALERT_LOW);
            local_tts_play("您当前血氧偏低，请暂停训练并休息哦。");
            break;
        default:
            break;
    }
}


static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static bool     s_time_valid = false;
static int64_t  s_time_epoch = 0;
static int64_t  s_time_sync_us = 0;
static int      s_tz_offset_min = -480;


static const char *action_to_str(int action)
{
    switch (action) {
        case 0:  return "trunk_bend";
        case 1:  return "side_raise";
        case 2:  return "elbow_curl";
        default: return "none";
    }
}

static const char *action_to_name(int action)
{
    switch (action) {
        case 0:  return "躯干侧屈";
        case 1:  return "侧平举";
        case 2:  return "屈伸肘";
        default: return "未知动作";
    }
}

static void json_begin(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t json_send(httpd_req_t *req, const char *body)
{
    json_begin(req);
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    return json_send(req, body);
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || req->content_len >= cap) return -1;
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return -1;
        received += (size_t)r;
    }
    buf[received] = '\0';
    return (int)received;
}


static void time_set(int64_t epoch, int offset_min)
{
    taskENTER_CRITICAL(&s_time_lock);
    s_time_epoch = epoch;
    s_time_sync_us = esp_timer_get_time();
    s_tz_offset_min = offset_min;
    s_time_valid = true;
    taskEXIT_CRITICAL(&s_time_lock);
}

static bool time_is_valid(void)
{
    bool valid;
    taskENTER_CRITICAL(&s_time_lock);
    valid = s_time_valid;
    taskEXIT_CRITICAL(&s_time_lock);

    return valid || time(NULL) > 1600000000;
}

static void time_now_parts(char *date, size_t date_len,
                           char *time_str, size_t time_len)
{
    int64_t utc;
    int offset;

    taskENTER_CRITICAL(&s_time_lock);
    if (s_time_valid) {
        utc = s_time_epoch + (esp_timer_get_time() - s_time_sync_us) / 1000000;
    } else {
        utc = (int64_t)time(NULL);
    }
    offset = s_tz_offset_min;
    taskEXIT_CRITICAL(&s_time_lock);

    time_t local = (time_t)(utc - (int64_t)offset * 60);
    struct tm tm_value;
    gmtime_r(&local, &tm_value);
    strftime(date, date_len, "%Y-%m-%d", &tm_value);
    strftime(time_str, time_len, "%H:%M:%S", &tm_value);
}


static bool history_lock(void)
{
    if (!s_history_mutex) return false;
    return xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

static void history_unlock(void)
{
    if (s_history_mutex) xSemaphoreGive(s_history_mutex);
}

static void history_save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, HIST_NVS_KEY, &s_store, sizeof(s_store));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History save failed: %s", esp_err_to_name(err));
    }
}

static void history_load(void)
{
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_store);
    esp_err_t err = nvs_get_blob(h, HIST_NVS_KEY, &s_store, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_store) ||
        s_store.magic != HIST_STORE_MAGIC ||
        s_store.version != HIST_STORE_VERSION) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = HIST_STORE_MAGIC;
        s_store.version = HIST_STORE_VERSION;
        return;
    }

    if (s_store.count > HIST_MAX) s_store.count = HIST_MAX;
    if (s_store.head >= HIST_MAX) s_store.head = 0;

    if (s_store.count > 0) {
        int last = (s_store.head - 1 + HIST_MAX) % HIST_MAX;
        strncpy(s_recorded_session, s_store.entries[last].session,
                sizeof(s_recorded_session) - 1);
        s_recorded_session[sizeof(s_recorded_session) - 1] = '\0';
    }
    ESP_LOGI(TAG, "History loaded: %u records", (unsigned)s_store.count);
}

static bool history_latest(history_entry_t *out)
{
    if (!out || !history_lock()) return false;
    bool ok = s_store.count > 0;
    if (ok) {
        int idx = (s_store.head - 1 + HIST_MAX) % HIST_MAX;
        *out = s_store.entries[idx];
    }
    history_unlock();
    return ok;
}


void web_server_session_started(const char *session)
{
    if (!session) return;
    taskENTER_CRITICAL(&s_sample_lock);
    s_sample_active = true;
    strncpy(s_sample_session, session, sizeof(s_sample_session) - 1);
    s_sample_session[sizeof(s_sample_session) - 1] = '\0';
    s_sample_start_us = esp_timer_get_time();
    s_sample_count = 0;
    s_sample_hr_sum = 0;
    s_sample_spo2_sum = 0;
    s_sample_hr_min = 0;
    s_sample_hr_max = 0;
    s_sample_spo2_min = 0;
    taskEXIT_CRITICAL(&s_sample_lock);


    phys_alert_reset();
    s_last_alert_us    = 0;
    s_first_action_us  = 0;

    ESP_LOGI(TAG, "Session sampling started: %s", session);
}

static void web_sampler_task(void *arg)
{
    (void)arg;
    while (1) {
        k230_status_t st;
        k230_get_status(&st);
        if (st.running && st.session[0]) {
            bool new_session = false;


            taskENTER_CRITICAL(&s_sample_lock);
            if (!s_sample_active || strcmp(s_sample_session, st.session) != 0) {
                s_sample_active = true;
                strncpy(s_sample_session, st.session, sizeof(s_sample_session) - 1);
                s_sample_session[sizeof(s_sample_session) - 1] = '\0';
                s_sample_start_us = esp_timer_get_time();
                s_sample_count = 0;
                s_sample_hr_sum = 0;
                s_sample_spo2_sum = 0;
                s_sample_hr_min = 0;
                s_sample_hr_max = 0;
                s_sample_spo2_min = 0;
                new_session = true;
            }
            taskEXIT_CRITICAL(&s_sample_lock);

            if (new_session) {
                phys_alert_reset();
                s_last_alert_us   = 0;
                s_first_action_us = 0;
            }

            int hr = g_watch_hr;
            int spo2 = g_watch_spo2;
            bool vitals_valid = g_watch_data_valid && hr > 0 && spo2 > 0;


            int64_t now_us = esp_timer_get_time();
            bool alert_ready = false;
            if (st.total > 0) {
                if (s_first_action_us == 0) s_first_action_us = now_us;
                alert_ready = (now_us - s_first_action_us) >=
                              (int64_t)ALERT_START_GRACE_SEC * 1000000LL;
            } else {
                s_first_action_us = 0;
            }
            phys_alert_evaluate(alert_ready, hr, spo2, vitals_valid);

            if (vitals_valid) {
                taskENTER_CRITICAL(&s_sample_lock);
                if (s_sample_active && strcmp(s_sample_session, st.session) == 0) {
                    s_sample_count++;
                    s_sample_hr_sum += (uint32_t)hr;
                    s_sample_spo2_sum += (uint32_t)spo2;
                    if (s_sample_hr_min == 0 || hr < s_sample_hr_min) s_sample_hr_min = hr;
                    if (hr > s_sample_hr_max) s_sample_hr_max = hr;
                    if (s_sample_spo2_min == 0 || spo2 < s_sample_spo2_min) s_sample_spo2_min = spo2;
                }
                taskEXIT_CRITICAL(&s_sample_lock);
            }
        } else {

            s_first_action_us = 0;
            phys_alert_evaluate(false, 0, 0, false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void web_server_record_snapshot(void)
{
    k230_status_t status;
    if (k230_get_report(&status) && !status.running) {
        web_server_record_training(&status);
    }
}

void web_server_record_training(const k230_status_t *status)
{
    if (!status || !status->session[0] || status->total <= 0) return;

    char date[12], time_str[12];
    time_now_parts(date, sizeof(date), time_str, sizeof(time_str));

    int32_t hr_avg = 0, hr_min = 0, hr_max = 0, spo2_avg = 0, spo2_min = 0;
    uint32_t duration_s = 0;
    uint32_t sample_count = 0;

    taskENTER_CRITICAL(&s_sample_lock);
    if (s_sample_active && strcmp(s_sample_session, status->session) == 0) {
        sample_count = s_sample_count;


        int64_t now_us = esp_timer_get_time();
        if (s_sample_start_us > 0 && now_us > s_sample_start_us) {
            duration_s = (uint32_t)((now_us - s_sample_start_us) / 1000000);
        }
        if (sample_count > 0) {
            hr_avg = (int32_t)(s_sample_hr_sum / sample_count);
            spo2_avg = (int32_t)(s_sample_spo2_sum / sample_count);
            hr_min = s_sample_hr_min;
            hr_max = s_sample_hr_max;
            spo2_min = s_sample_spo2_min;
        }
        s_sample_active = false;
    }
    taskEXIT_CRITICAL(&s_sample_lock);


    if (sample_count == 0) {
        hr_avg = hr_min = hr_max = g_watch_hr;
        spo2_avg = spo2_min = g_watch_spo2;
    }


    if (status->total > 0 && duration_s == 0) {
        duration_s = 1;
    }

    if (!history_lock()) return;

    if (strcmp(s_recorded_session, status->session) == 0) {
        history_unlock();
        return;
    }
    strncpy(s_recorded_session, status->session, sizeof(s_recorded_session) - 1);
    s_recorded_session[sizeof(s_recorded_session) - 1] = '\0';

    history_entry_t *e = &s_store.entries[s_store.head];
    memset(e, 0, sizeof(*e));
    strncpy(e->date_str, date, sizeof(e->date_str) - 1);
    strncpy(e->time_str, time_str, sizeof(e->time_str) - 1);
    strncpy(e->session, status->session, sizeof(e->session) - 1);
    e->action       = status->action;
    e->total        = status->total;
    e->standard     = status->standard;
    e->nonstandard  = status->nonstandard;
    e->incomplete   = status->incomplete;
    e->interrupted  = status->interrupted;
    e->short_hold   = status->short_hold;
    e->score        = status->score;
    e->score_sum    = status->score_sum;
    e->hr_avg       = hr_avg;
    e->hr_min       = hr_min;
    e->hr_max       = hr_max;
    e->spo2_avg     = spo2_avg;
    e->spo2_min     = spo2_min;
    e->duration_s   = duration_s;
    for (int i = 0; i < HIST_ERR_COUNT; i++) e->errors[i] = status->errors[i];

    s_store.head = (uint16_t)((s_store.head + 1) % HIST_MAX);
    if (s_store.count < HIST_MAX) s_store.count++;

    history_save_locked();
    history_unlock();

    ESP_LOGI(TAG, "History +1: %s %s total=%d qual=%d rate=%d%% hr=%d spo2=%d dur=%us",
             date, time_str, (int)status->total, (int)status->standard,
             status->total ? (int)(status->standard * 100 / status->total) : 0,
             (int)hr_avg, (int)spo2_avg, (unsigned)duration_s);
}


static esp_err_t index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "--- HTTP GET %s ---", req->uri);
    size_t len = (size_t)(dashboard_html_end - dashboard_html_start);
    if (len > 0 && dashboard_html_start[len - 1] == 0) len--;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)dashboard_html_start, len);
}


static esp_err_t api_data_handler(httpd_req_t *req)
{
    k230_status_t st;
    bool have_session = k230_get_status(&st);
    int avg_score = st.total > 0 ? st.score_sum / st.total : 0;

    char buf[1536];
    int n = snprintf(buf, sizeof(buf),
        "{\"heart_rate\":%d,\"spo2\":%d,"
        "\"movement\":\"%s\",\"action\":%d,"
        "\"total\":%d,\"qualified\":%d,\"unqualified\":%d,"
        "\"running\":%s,\"confirmed\":%s,\"connected\":%s,"
        "\"watch_connected\":%s,"
        "\"score\":%d,\"score_sum\":%d,\"avg_score\":%d,"
        "\"feedback\":%d,\"incomplete\":%d,\"interrupted\":%d,"
        "\"short_hold\":%d,"
        "\"session\":\"%s\",\"state\":\"%s\",\"idle_timeout\":%s,"
        "\"errors\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
        g_watch_hr, g_watch_spo2,
        have_session ? action_to_str(st.action) : "none",
        have_session ? st.action : -1,
        st.total, st.standard, st.nonstandard,
        st.running ? "true" : "false",
        st.confirmed ? "true" : "false",
        st.connected ? "true" : "false",
        espnow_comm_watch_alive() ? "true" : "false",
        st.score, st.score_sum, avg_score,
        st.feedback, st.incomplete, st.interrupted, st.short_hold,
        have_session ? st.session : "", have_session ? st.state : "IDLE",
        st.idle_timeout ? "true" : "false",
        st.errors[0], st.errors[1], st.errors[2], st.errors[3], st.errors[4],
        st.errors[5], st.errors[6], st.errors[7], st.errors[8], st.errors[9]);

    if (n < 0 || n >= (int)sizeof(buf)) {
        return json_error(req, "500 Internal Server Error", "data too long");
    }
    return json_send(req, buf);
}


static esp_err_t api_history_handler(httpd_req_t *req)
{
    char *buf = malloc(8192);
    if (!buf) return json_error(req, "500 Internal Server Error", "oom");

    int pos = 0;
    buf[pos++] = '[';

    if (!history_lock()) {
        free(buf);
        return json_error(req, "500 Internal Server Error", "history busy");
    }

    int printed = 0;
    for (int i = 0; i < s_store.count; i++) {
        int idx = (s_store.head - 1 - i + HIST_MAX * 2) % HIST_MAX;
        history_entry_t *e = &s_store.entries[idx];
        if (e->total <= 0) continue;

        int start = pos;
        if (printed > 0) {
            if (pos + 3 >= 8192) break;
            buf[pos++] = ',';
        }
        size_t remain = 8192 - (size_t)pos;
        int m = snprintf(buf + pos, remain,
            "{\"date\":\"%s\",\"time\":\"%s\",\"session\":\"%s\","
            "\"action\":\"%s\",\"action_id\":%d,"
            "\"total\":%d,\"qualified\":%d,\"unqualified\":%d,"
            "\"incomplete\":%d,\"interrupted\":%d,\"short_hold\":%d,"
            "\"score\":%d,\"score_sum\":%d,"
            "\"hr_avg\":%d,\"hr_min\":%d,\"hr_max\":%d,"
            "\"spo2_avg\":%d,\"spo2_min\":%d,\"duration_s\":%u,"
            "\"errors\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
            e->date_str, e->time_str, e->session,
            action_to_str(e->action), (int)e->action,
            (int)e->total, (int)e->standard, (int)e->nonstandard,
            (int)e->incomplete, (int)e->interrupted, (int)e->short_hold,
            (int)e->score, (int)e->score_sum,
            (int)e->hr_avg, (int)e->hr_min, (int)e->hr_max,
            (int)e->spo2_avg, (int)e->spo2_min, (unsigned)e->duration_s,
            (int)e->errors[0], (int)e->errors[1], (int)e->errors[2], (int)e->errors[3],
            (int)e->errors[4], (int)e->errors[5], (int)e->errors[6], (int)e->errors[7],
            (int)e->errors[8], (int)e->errors[9]);
        if (m < 0 || (size_t)m >= remain) {

            pos = start;
            buf[pos] = '\0';
            break;
        }
        pos += m;
        printed++;
        if (pos >= 8192 - 640) break;
    }

    history_unlock();
    if (pos > 8192 - 2) pos = 8192 - 2;
    buf[pos++] = ']';
    buf[pos] = '\0';

    esp_err_t ret = json_send(req, buf);
    free(buf);
    return ret;
}


static esp_err_t api_history_delete_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return json_error(req, "400 Bad Request", "invalid json");

    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    if (!cJSON_IsString(session) || !session->valuestring || !session->valuestring[0]) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "session required");
    }
    char session_buf[24];
    strncpy(session_buf, session->valuestring, sizeof(session_buf) - 1);
    session_buf[sizeof(session_buf) - 1] = '\0';
    cJSON_Delete(root);

    if (!history_lock()) {
        return json_error(req, "500 Internal Server Error", "history busy");
    }

    int found = -1;
    for (int i = 0; i < s_store.count; i++) {
        int idx = (s_store.head - 1 - i + HIST_MAX * 2) % HIST_MAX;
        if (strcmp(s_store.entries[idx].session, session_buf) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        history_unlock();
        return json_error(req, "404 Not Found", "record not found");
    }


    for (int j = found; j < s_store.count - 1; j++) {
        int dst = (s_store.head - 1 - j + HIST_MAX * 2) % HIST_MAX;
        int src = (s_store.head - 2 - j + HIST_MAX * 2) % HIST_MAX;
        s_store.entries[dst] = s_store.entries[src];
    }
    int last = (s_store.head - s_store.count + HIST_MAX * 2) % HIST_MAX;
    memset(&s_store.entries[last], 0, sizeof(history_entry_t));
    if (s_store.count > 0) s_store.count--;

    uint16_t remaining = s_store.count;
    history_save_locked();
    history_unlock();

    ESP_LOGI(TAG, "History -1: session=%s, %u left", session_buf, (unsigned)remaining);

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"removed\":1,\"count\":%u}",
             (unsigned)remaining);
    return json_send(req, resp);
}


static esp_err_t api_stats_handler(httpd_req_t *req)
{
    int sessions[3] = {0}, total[3] = {0}, qual[3] = {0}, unqual[3] = {0};
    int score_sum[3] = {0}, durations[3] = {0};

    if (!history_lock()) {
        return json_error(req, "500 Internal Server Error", "history busy");
    }
    for (int i = 0; i < s_store.count; i++) {
        int idx = (s_store.head - 1 - i + HIST_MAX * 2) % HIST_MAX;
        history_entry_t *e = &s_store.entries[idx];
        int a = e->action;
        if (a < 0 || a > 2) continue;
        sessions[a]++;
        total[a] += e->total;
        qual[a] += e->standard;
        unqual[a] += e->nonstandard;
        score_sum[a] += e->score_sum;
        durations[a] += (int)e->duration_s;
    }
    history_unlock();

    int all_sessions = sessions[0] + sessions[1] + sessions[2];
    int all_total = total[0] + total[1] + total[2];
    int all_qual = qual[0] + qual[1] + qual[2];
    int all_unqual = unqual[0] + unqual[1] + unqual[2];
    int all_score = score_sum[0] + score_sum[1] + score_sum[2];
    int all_duration = durations[0] + durations[1] + durations[2];

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"sessions\":%d,\"total\":%d,\"qualified\":%d,\"unqualified\":%d,"
        "\"score_sum\":%d,\"duration_s\":%d,"
        "\"actions\":{"
        "\"trunk_bend\":{\"sessions\":%d,\"total\":%d,\"qualified\":%d,\"unqualified\":%d,\"score_sum\":%d,\"duration_s\":%d},"
        "\"side_raise\":{\"sessions\":%d,\"total\":%d,\"qualified\":%d,\"unqualified\":%d,\"score_sum\":%d,\"duration_s\":%d},"
        "\"elbow_curl\":{\"sessions\":%d,\"total\":%d,\"qualified\":%d,\"unqualified\":%d,\"score_sum\":%d,\"duration_s\":%d}"
        "}}",
        all_sessions, all_total, all_qual, all_unqual, all_score, all_duration,
        sessions[0], total[0], qual[0], unqual[0], score_sum[0], durations[0],
        sessions[1], total[1], qual[1], unqual[1], score_sum[1], durations[1],
        sessions[2], total[2], qual[2], unqual[2], score_sum[2], durations[2]);

    if (n < 0 || n >= (int)sizeof(buf)) {
        return json_error(req, "500 Internal Server Error", "stats too long");
    }
    return json_send(req, buf);
}


static esp_err_t api_report_handler(httpd_req_t *req)
{
    history_entry_t e;
    bool have = history_latest(&e);

    if (!have) {

        k230_status_t st;
        if (k230_get_report(&st) && st.total > 0) {
            memset(&e, 0, sizeof(e));
            time_now_parts(e.date_str, sizeof(e.date_str),
                           e.time_str, sizeof(e.time_str));
            strncpy(e.session, st.session, sizeof(e.session) - 1);
            e.action = st.action;
            e.total = st.total;
            e.standard = st.standard;
            e.nonstandard = st.nonstandard;
            e.incomplete = st.incomplete;
            e.interrupted = st.interrupted;
            e.short_hold = st.short_hold;
            e.score = st.score;
            e.score_sum = st.score_sum;
            e.hr_avg = e.hr_min = e.hr_max = g_watch_hr;
            e.spo2_avg = e.spo2_min = g_watch_spo2;
            for (int i = 0; i < HIST_ERR_COUNT; i++) e.errors[i] = st.errors[i];
            have = true;
        }
    }

    if (!have) {
        return json_send(req, "{\"has_report\":false}");
    }

    int avg_score = e.total > 0 ? e.score_sum / e.total : e.score;
    char buf[1280];
    int n = snprintf(buf, sizeof(buf),
        "{\"has_report\":true,"
        "\"date\":\"%s\",\"time\":\"%s\",\"session\":\"%s\","
        "\"action\":\"%s\",\"action_id\":%d,"
        "\"total\":%d,\"qualified\":%d,\"unqualified\":%d,"
        "\"incomplete\":%d,\"interrupted\":%d,\"short_hold\":%d,"
        "\"score\":%d,\"score_sum\":%d,\"avg_score\":%d,"
        "\"hr_avg\":%d,\"hr_min\":%d,\"hr_max\":%d,"
        "\"spo2_avg\":%d,\"spo2_min\":%d,\"duration_s\":%u,"
        "\"errors\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
        e.date_str, e.time_str, e.session,
        action_to_str(e.action), (int)e.action,
        (int)e.total, (int)e.standard, (int)e.nonstandard,
        (int)e.incomplete, (int)e.interrupted, (int)e.short_hold,
        (int)e.score, (int)e.score_sum, avg_score,
        (int)e.hr_avg, (int)e.hr_min, (int)e.hr_max,
        (int)e.spo2_avg, (int)e.spo2_min, (unsigned)e.duration_s,
        (int)e.errors[0], (int)e.errors[1], (int)e.errors[2], (int)e.errors[3],
        (int)e.errors[4], (int)e.errors[5], (int)e.errors[6], (int)e.errors[7],
        (int)e.errors[8], (int)e.errors[9]);

    if (n < 0 || n >= (int)sizeof(buf)) {
        return json_error(req, "500 Internal Server Error", "report too long");
    }
    return json_send(req, buf);
}


static esp_err_t api_system_handler(httpd_req_t *req)
{
    char sta_ip[16] = {0};
    wifi_get_sta_ip(sta_ip, sizeof(sta_ip));
    bool sta = is_wifi_connected();

    k230_status_t st;
    k230_get_status(&st);

    int64_t uptime = esp_timer_get_time() / 1000000;

    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%lld,\"heap_free\":%u,\"heap_total\":%u,"
        "\"psram_free\":%u,\"psram_total\":%u,"
        "\"sta_connected\":%s,\"sta_ip\":\"%s\",\"ap_ssid\":\"%s\","
        "\"k230_connected\":%s,\"watch_connected\":%s,\"time_synced\":%s}",
        (long long)uptime,
        (unsigned)heap_free, (unsigned)heap_total,
        (unsigned)psram_free, (unsigned)psram_total,
        sta ? "true" : "false", sta_ip, AP_SSID,
        st.connected ? "true" : "false",
        espnow_comm_watch_alive() ? "true" : "false",
        time_is_valid() ? "true" : "false");

    if (n < 0 || n >= (int)sizeof(buf)) {
        return json_error(req, "500 Internal Server Error", "system too long");
    }
    return json_send(req, buf);
}


static esp_err_t api_time_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return json_error(req, "400 Bad Request", "invalid json");

    cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "epoch");
    cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
    if (!cJSON_IsNumber(epoch) || !cJSON_IsNumber(offset)) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "epoch/offset required");
    }

    int64_t epoch_v = (int64_t)epoch->valuedouble;
    int offset_v = offset->valueint;
    cJSON_Delete(root);

    if (epoch_v < 1600000000LL || epoch_v > 4102444800LL ||
        offset_v < -840 || offset_v > 840) {
        return json_error(req, "400 Bad Request", "time out of range");
    }

    time_set(epoch_v, offset_v);
    ESP_LOGI(TAG, "Time synced by browser: epoch=%lld offset=%d",
             (long long)epoch_v, offset_v);

    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"epoch\":%lld,\"offset\":%d}",
             (long long)epoch_v, offset_v);
    return json_send(req, resp);
}


static esp_err_t api_control_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return json_error(req, "400 Bad Request", "invalid json");

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd) || !cmd->valuestring) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "cmd required");
    }

    char cmd_buf[16];
    strncpy(cmd_buf, cmd->valuestring, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    if (strcmp(cmd_buf, "select") == 0) {
        cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsNumber(action) || action->valueint < 0 || action->valueint > 2) {
            cJSON_Delete(root);
            return json_error(req, "400 Bad Request", "action 0/1/2 required");
        }
        int selected = action->valueint;
        cJSON_Delete(root);

        tft_web_ui_request(3, selected);
        char resp[96];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"cmd\":\"select\",\"action\":%d}", selected);
        return json_send(req, resp);
    }

    if (strcmp(cmd_buf, "stop") == 0) {
        cJSON_Delete(root);
        k230_training_stop();

        tft_web_ui_request(5, -1);
        return json_send(req, "{\"ok\":true,\"cmd\":\"stop\"}");
    }

    if (strcmp(cmd_buf, "start") != 0) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "unknown cmd");
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsNumber(action) || action->valueint < 0 || action->valueint > 2) {
        cJSON_Delete(root);
        return json_error(req, "400 Bad Request", "action 0/1/2 required");
    }
    int action_v = action->valueint;
    cJSON_Delete(root);

    k230_status_t st;
    k230_get_status(&st);
    if (!st.connected) {
        return json_error(req, "503 Service Unavailable", "K230 offline");
    }

    if (!k230_training_start(action_v)) {
        return json_error(req, "409 Conflict", "K230 busy or previous session not confirmed");
    }


    tft_web_ui_request(4, action_v);

    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"cmd\":\"start\",\"action\":%d,\"action_name\":\"%s\"}",
             action_v, action_to_name(action_v));
    return json_send(req, resp);
}


static void html_escape(char *dst, size_t dst_len, const char *src)
{
    size_t d = 0;
    if (!dst || dst_len == 0) return;
    for (const char *s = src; *s && d + 7 < dst_len; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            memcpy(dst + d, rep, rl);
            d += rl;
        } else {
            dst[d++] = *s;
        }
    }
    dst[d] = '\0';
}

static bool url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t d = 0;
    if (!dst || dst_len == 0) return false;
    dst[0] = '\0';
    for (size_t i = 0; i < src_len; i++) {
        unsigned char value = (unsigned char)src[i];
        if (value == '+') value = ' ';
        else if (value == '%') {
            if (i + 2 >= src_len || !isxdigit((unsigned char)src[i + 1])
                                || !isxdigit((unsigned char)src[i + 2])) return false;
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            value = (unsigned char)strtol(hex, NULL, 16);
            i += 2;
        }
        if (value == 0 || d + 1 >= dst_len) return false;
        dst[d++] = (char)value;
    }
    dst[d] = '\0';
    return true;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || !out_len) return false;
    out[0] = '\0';
    size_t key_len = strlen(key);
    for (const char *field = body; *field;) {
        const char *end = strchr(field, '&');
        if (!end) end = field + strlen(field);
        const char *eq = memchr(field, '=', (size_t)(end - field));
        if (eq && (size_t)(eq - field) == key_len && !memcmp(field, key, key_len))
            return url_decode(out, out_len, eq + 1, (size_t)(end - eq - 1));
        field = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "--- HTTP GET %s ---", req->uri);
    char ssid[33] = {0}, pass[65] = {0}, ip[16] = {0};
    char ssid_h[200], pass_h[300];
    wifi_get_router_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    wifi_get_sta_ip(ip, sizeof(ip));
    html_escape(ssid_h, sizeof(ssid_h), ssid);
    html_escape(pass_h, sizeof(pass_h), pass);

    bool up = is_wifi_connected();

    char *html = malloc(6144);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    snprintf(html, 6144,
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>WiFi 设置</title><style>"
        "body{font-family:system-ui,-apple-system,'Microsoft YaHei',sans-serif;"
        "background:#0d1f18;color:#e8f5e9;margin:0;padding:18px;}"
        ".card{max-width:460px;margin:0 auto;background:#14342a;border:1px solid #2e6b4f;"
        "border-radius:12px;padding:22px;}"
        "h1{font-size:20px;margin:0 0 16px;}"
        ".st{background:#0d1f18;border-radius:8px;padding:12px;margin-bottom:18px;"
        "font-size:14px;line-height:1.8;word-break:break-all;}"
        ".ok{color:#7ee787;}.bad{color:#ff7b72;}"
        "label{display:block;font-size:14px;margin:14px 0 6px;color:#a5d6a7;}"
        "input{width:100%%;box-sizing:border-box;padding:11px;font-size:16px;"
        "border-radius:8px;border:1px solid #2e6b4f;background:#0d1f18;color:#e8f5e9;}"
        "button{width:100%%;margin-top:20px;padding:13px;font-size:16px;font-weight:600;"
        "border:0;border-radius:8px;background:#2e7d5b;color:#fff;}"
        ".tip{font-size:12.5px;color:#8aa79b;line-height:1.75;margin-top:18px;}"
        "a{color:#7ee787;}"
        "</style></head><body><div class=\"card\">"
        "<h1>WiFi 设置</h1>"
        "<div class=\"st\">"
        "状态：%s%s<br>"
        "已保存热点：<b>%s</b><br>"
        "密码：<b>%s</b><br>"
        "上次失败原因：<b>%s</b>"
        "</div>"
        "<form method=\"POST\" action=\"/api/wifi\">"
        "<label>热点名称（SSID）</label>"
        "<input name=\"ssid\" value=\"%s\" maxlength=\"32\" required>"
        "<label>密码（开放热点留空）</label>"
        "<input name=\"pass\" value=\"%s\" maxlength=\"64\">"
        "<button type=\"submit\">保存并重连</button>"
        "</form>"
        "<div class=\"tip\">"
        "&#9888; 只能连 <b>2.4GHz</b> 的热点，ESP32 不支持 5GHz。<br>"
        "&#9888; 手机热点请在「热点设置」里把 AP 频段选成 <b>2.4GHz</b>，"
        "并把热点保持开启——手机息屏后有些机型会自动关闭热点。<br>"
        "&#9888; 保存后板子会切到新信道去连热点，本页面可能会短暂断开，"
        "这是正常的；重连后回到本页即可看到结果。"
        "</div>"
        "<p><a href=\"/scan\">&#128225; 扫描附近热点（看 ESP32 眼里的加密方式）</a></p>"
        "<p><a href=\"/\">&#8592; 返回仪表盘</a></p>"
        "</div></body></html>",
        up ? "<span class=\"ok\">已连接</span>" : "<span class=\"bad\">未连接</span>",
        up ? ip : "",
        ssid_h,
        pass_h[0] ? pass_h : "(无)",
        wifi_get_last_reason_str(),
        ssid_h,
        pass_h);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, html, strlen(html));
    free(html);
    return r;
}

static esp_err_t api_wifi_set_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    if (memchr(body, '\0', received) ||
        !form_get(body, "ssid", ssid, sizeof(ssid)) ||
        !form_get(body, "pass", pass, sizeof(pass))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid/password form");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "--- HTTP POST /api/wifi (%d bytes) ssid=\"%s\" ---",
             received, ssid);
    free(body);

    esp_err_t ret = wifi_set_router_credentials(ssid, pass);
    ESP_LOGI(TAG, "WiFi config from web: ssid=\"%s\" -> %s",
             ssid, esp_err_to_name(ret));

    char page[640];
    int n = snprintf(page, sizeof(page),
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"6;url=/wifi\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>已保存</title></head>"
        "<body style=\"font-family:system-ui,'Microsoft YaHei',sans-serif;"
        "background:#0d1f18;color:#e8f5e9;padding:24px;line-height:1.8\">"
        "<h2>%s</h2><p>热点：<b>%s</b></p>"
        "<p>正在重连，6 秒后自动刷新（若页面打不开，请重新连回 "
        "<b>%s</b> 并访问 <b>http://192.168.4.1/wifi</b>）。</p>"
        "</body></html>",
        ret == ESP_OK ? "已保存，正在重连…" : "保存失败",
        ssid, AP_SSID);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, n);
    return ESP_OK;
}

static esp_err_t scan_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "--- HTTP GET /scan → 开始扫描 ---");

    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;

    char *html = malloc(6144);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_wifi_scan_start(&sc, true);
    if (ret != ESP_OK) {
        snprintf(html, 6144,
            "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<title>扫描失败</title></head><body style=\"font-family:sans-serif;"
            "background:#0d1f18;color:#e8f5e9;padding:24px\">"
            "<h2>扫描失败</h2><p>%s</p><p><a style=\"color:#7ee787\" "
            "href=\"/wifi\">← 返回</a></p></body></html>", esp_err_to_name(ret));
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, html, strlen(html));
        free(html);
        return ESP_OK;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "扫描到 %u 个热点", (unsigned)n);
    if (n > 25) n = 25;
    wifi_ap_record_t *list = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!list) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    if (esp_wifi_scan_get_ap_records(&n, list) != ESP_OK) n = 0;

    int pos = snprintf(html, 6144,
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>附近热点</title><style>"
        "body{font-family:system-ui,'Microsoft YaHei',sans-serif;background:#0d1f18;"
        "color:#e8f5e9;margin:0;padding:16px;font-size:14px}"
        "h2{font-size:19px}table{width:100%%;border-collapse:collapse;margin-top:12px}"
        "td,th{padding:8px 6px;border-bottom:1px solid #2e6b4f;text-align:left}"
        "th{color:#a5d6a7;font-weight:600}"
        ".bad{color:#ff7b72}.ok{color:#7ee787}"
        "a{color:#7ee787}small{color:#8aa79b}"
        "</style></head><body><h2>ESP32 扫到的 %u 个热点</h2>"
        "<table><tr><th>名称</th><th>加密</th><th>信道</th><th>信号</th></tr>",
        (unsigned)n);

    for (int i = 0; i < n; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s", (char *)list[i].ssid);
        for (char *p = name; *p; p++) if ((unsigned char)*p < 0x20) *p = '?';

        bool bad = (list[i].authmode == WIFI_AUTH_OPEN ||
                    list[i].authmode == WIFI_AUTH_WPA3_PSK ||
                    list[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK);
        pos += snprintf(html + pos, 6144 - pos,
            "<tr><td><b>%s</b></td><td class=\"%s\">%s</td><td>%d</td><td>%d dBm</td></tr>",
            name, bad ? "bad" : "ok", wifi_auth_mode_str(list[i].authmode),
            list[i].primary, list[i].rssi);
        if (pos >= 6144 - 256) break;
    }

    pos += snprintf(html + pos, 6144 - pos,
        "</table><p><small>红色 = 这些加密方式 ESP32 可能连不上"
        "（开放网络没有密码；WPA3 若热点是纯 WPA3 模式需要改回 WPA2）。</small></p>"
        "<p><small>看不到你的热点 = 它不在 2.4GHz，或者离得太远。</small></p>"
        "<p><a href=\"/wifi\">← 返回 WiFi 设置</a></p></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, pos);
    free(list);
    free(html);
    return ESP_OK;
}

static esp_err_t catchall_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "--- HTTP %s %s → 404 ---", req->method == HTTP_GET ? "GET" : "OTHER", req->uri);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "404");
    return ESP_FAIL;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


void web_server_init(void)
{
    s_history_mutex = xSemaphoreCreateMutex();
    if (!s_history_mutex) {
        ESP_LOGE(TAG, "Failed to create history mutex");
    } else {
        history_load();
    }

    BaseType_t task_ok = xTaskCreate(web_sampler_task, "web_sampler", 4096,
                                     NULL, 2, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create web sampler task");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler,        .user_ctx = NULL },
        { .uri = "/api/data",    .method = HTTP_GET,  .handler = api_data_handler,     .user_ctx = NULL },
        { .uri = "/api/history", .method = HTTP_GET,  .handler = api_history_handler,  .user_ctx = NULL },
        { .uri = "/api/history/delete", .method = HTTP_POST, .handler = api_history_delete_handler, .user_ctx = NULL },
        { .uri = "/api/stats",   .method = HTTP_GET,  .handler = api_stats_handler,    .user_ctx = NULL },
        { .uri = "/api/report",  .method = HTTP_GET,  .handler = api_report_handler,   .user_ctx = NULL },
        { .uri = "/api/system",  .method = HTTP_GET,  .handler = api_system_handler,   .user_ctx = NULL },
        { .uri = "/api/time",    .method = HTTP_POST, .handler = api_time_handler,     .user_ctx = NULL },
        { .uri = "/api/control", .method = HTTP_POST, .handler = api_control_handler,  .user_ctx = NULL },
        { .uri = "/wifi",        .method = HTTP_GET,  .handler = wifi_page_handler,    .user_ctx = NULL },
        { .uri = "/scan",        .method = HTTP_GET,  .handler = scan_page_handler,    .user_ctx = NULL },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = api_wifi_set_handler, .user_ctx = NULL },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = favicon_handler,      .user_ctx = NULL },
        { .uri = "/*",           .method = HTTP_GET,  .handler = catchall_handler,     .user_ctx = NULL },
    };

    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Dashboard: http://192.168.4.1");
    ESP_LOGI(TAG, "WiFi setup: http://192.168.4.1/wifi");
}
