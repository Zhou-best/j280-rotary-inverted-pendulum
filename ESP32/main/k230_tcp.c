#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "k230_tcp.h"
#include "fpga_link.h"
#include "uart_startup.h"
#include "espnow_comm.h"
#include "web_server.h"
#include "tft_spi.h"

static const char *TAG = "K230_TCP";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static k230_status_t s_status;
static uint32_t s_boot, s_session_number;
static bool s_requested_running;


static bool s_training_started;
static int64_t s_last_rx;


static int64_t s_stop_requested_at;

static const char *action_name(int action)
{
    switch (action) {
    case ACTION_TRUNK_BEND: return "trunk_bend";
    case ACTION_SIDE_RAISE: return "side_raise";
    case ACTION_ELBOW_CURL: return "elbow_curl";
    default: return NULL;
    }
}

bool k230_training_start(int action)
{
    if (!action_name(action)) {
        ESP_LOGW(TAG, "Unsupported K230 action");
        return false;
    }
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);


    bool k230_online = s_last_rx && (now - s_last_rx < 6000000);
    bool awaiting_ack = s_training_started && s_status.session[0] && !s_status.confirmed;
    bool stop_recent = s_stop_requested_at && (now - s_stop_requested_at < 3000000);
    if (awaiting_ack && k230_online && stop_recent) {
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Waiting for previous command acknowledgement (K230 online)");
        return false;
    }
    if (s_requested_running) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    s_stop_requested_at = 0;
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.session, sizeof(s_status.session), "%08lx-%08lx",
             (unsigned long)s_boot, (unsigned long)++s_session_number);
    s_training_started = true;
    s_requested_running = true;
    s_status.action = action;
    strcpy(s_status.state, "STARTING");
    g_k230_action = action;
    g_k230_total = g_k230_standard = g_k230_nonstandard = 0;
    char started_session[24];
    strncpy(started_session, s_status.session, sizeof(started_session) - 1);
    started_session[sizeof(started_session) - 1] = '\0';
    taskEXIT_CRITICAL(&s_lock);


    web_server_session_started(started_session);


    if (awaiting_ack) {
        ESP_LOGW(TAG, "Previous session not acknowledged (K230 %s, stop %s) — starting new session",
                 k230_online ? "online" : "offline",
                 stop_recent ? "recent" : "aged");
    }
    ESP_LOGI(TAG, "Requested %s START session=%s; waiting for K230 reset", action_name(action), s_status.session);
    return true;
}

void k230_training_stop(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_requested_running) {
        s_requested_running = false;
        s_status.confirmed = false;
        s_stop_requested_at = esp_timer_get_time();
        strcpy(s_status.state, "STOPPING");
    }
    g_k230_total = g_k230_standard = g_k230_nonstandard = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Requested STOP; live counts cleared, awaiting final report data");
}

bool k230_get_status(k230_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;

    if (!s_requested_running) {
        out->running = false;
        out->total = out->standard = out->nonstandard = 0;
        out->score = out->feedback = out->score_sum = 0;
        out->incomplete = out->interrupted = out->short_hold = 0;
        memset(out->errors, 0, sizeof(out->errors));
    }
    out->connected = s_last_rx && (esp_timer_get_time() - s_last_rx < 6000000);
    taskEXIT_CRITICAL(&s_lock);

    return s_training_started && out->session[0] != '\0';
}

bool k230_get_report(k230_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
    return s_training_started && out->session[0] != '\0' && out->confirmed;
}

static bool get_int(const cJSON *json, const char *key, int max, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > max ||
        item->valuedouble != item->valueint) return false;
    *out = item->valueint;
    return true;
}


static void mark_link_rx(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_last_rx = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
}


static void handle_ui(const cJSON *json)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (!cJSON_IsString(cmd)) return;
    int act = -1;
    if (cJSON_IsString(action)) {
        if (!strcmp(action->valuestring, "trunk_bend")) act = 0;
        else if (!strcmp(action->valuestring, "side_raise")) act = 1;
        else if (!strcmp(action->valuestring, "elbow_curl")) act = 2;
    }
    if (!strcmp(cmd->valuestring, "select") && act >= 0) {
        tft_k230_ui_request(0, act);
        ESP_LOGI(TAG, "UI select action %d", act);
    } else if (!strcmp(cmd->valuestring, "start") && act >= 0) {
        tft_k230_ui_request(1, act);
        ESP_LOGI(TAG, "UI start action %d", act);
    } else if (!strcmp(cmd->valuestring, "stop")) {
        tft_k230_ui_request(2, -1);
        ESP_LOGI(TAG, "UI stop");
    }
}

static void parse_status(const char *line)
{
    cJSON *json = cJSON_ParseWithOpts(line, NULL, true);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return; }
    mark_link_rx();
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");


    if (cJSON_IsString(type) && !strcmp(type->valuestring, "cmd")) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
        const cJSON *arg = cJSON_GetObjectItemCaseSensitive(json, "arg");
        if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "gesture") &&
            cJSON_IsString(arg)) {
            fpga_link_handle_gesture(arg->valuestring);
        }
        cJSON_Delete(json);
        return;
    }

    if (cJSON_IsString(type) && !strcmp(type->valuestring, "ui")) {
        handle_ui(json);
        cJSON_Delete(json);
        return;
    }
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(json, "session");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    const cJSON *running = cJSON_GetObjectItemCaseSensitive(json, "running");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "state");
    const cJSON *stop_reason = cJSON_GetObjectItemCaseSensitive(json, "stop_reason");
    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(json, "errors");
    k230_status_t next = {0};
    bool ok = cJSON_IsString(type) && !strcmp(type->valuestring, "status") &&
        cJSON_IsString(session) && strlen(session->valuestring) < sizeof(next.session) &&
        cJSON_IsString(action) &&
        cJSON_IsBool(running) && cJSON_IsString(state) &&
        (!stop_reason || (cJSON_IsString(stop_reason) &&
          (!stop_reason->valuestring[0] || !strcmp(stop_reason->valuestring, "idle_timeout") ||
           !strcmp(stop_reason->valuestring, "user_stop")))) &&
        strlen(state->valuestring) < sizeof(next.state) &&
        get_int(json, "seq", 2000000000, &next.seq) && next.seq > 0 &&
        get_int(json, "total", 65535, &next.total) &&
        get_int(json, "standard", 65535, &next.standard) &&
        get_int(json, "nonstandard", 65535, &next.nonstandard) &&
        next.total == next.standard + next.nonstandard &&
        get_int(json, "score", 100, &next.score) &&
        get_int(json, "feedback", 9, &next.feedback) &&
        get_int(json, "incomplete", 65535, &next.incomplete) &&
        get_int(json, "interrupted", 65535, &next.interrupted) &&
        get_int(json, "short_hold", 65535, &next.short_hold) &&
        get_int(json, "score_sum", 6553500, &next.score_sum) &&
        cJSON_IsArray(errors) && cJSON_GetArraySize(errors) == 10;
    for (int i = 0; ok && i < 10; ++i) {
        const cJSON *item = cJSON_GetArrayItem(errors, i);
        ok = cJSON_IsNumber(item) && item->valuedouble >= 0 &&
             item->valuedouble <= 65535 && item->valuedouble == item->valueint;
        if (ok) next.errors[i] = item->valueint;
    }
    if (!ok) {
        ESP_LOGW(TAG, "Rejected malformed status");
        cJSON_Delete(json);
        return;
    }
    next.action = !strcmp(action->valuestring, "trunk_bend") ? ACTION_TRUNK_BEND :
                  !strcmp(action->valuestring, "side_raise") ? ACTION_SIDE_RAISE :
                  !strcmp(action->valuestring, "elbow_curl") ? ACTION_ELBOW_CURL : -1;
    if (next.action < 0) { cJSON_Delete(json); return; }
    strcpy(next.session, session->valuestring);
    strcpy(next.state, state->valuestring);
    next.running = cJSON_IsTrue(running);
    next.idle_timeout = cJSON_IsString(stop_reason) && !strcmp(stop_reason->valuestring, "idle_timeout");


    next.user_stop = cJSON_IsString(stop_reason) && !strcmp(stop_reason->valuestring, "user_stop");
    if ((next.idle_timeout || next.user_stop) && (next.running || strcmp(next.state, "STOPPED"))) {
        cJSON_Delete(json);
        return;
    }
    next.confirmed = true;
    cJSON_Delete(json);

    taskENTER_CRITICAL(&s_lock);
    bool accept = next.action == s_status.action && !strcmp(next.session, s_status.session) &&
        (next.running == s_requested_running || next.idle_timeout || next.user_stop) &&
        next.seq > s_status.seq &&
        next.total >= s_status.total && next.standard >= s_status.standard &&
        next.nonstandard >= s_status.nonstandard && next.incomplete >= s_status.incomplete &&
        next.interrupted >= s_status.interrupted && next.short_hold >= s_status.short_hold &&
        next.score_sum >= s_status.score_sum;
    for (int i = 0; accept && i < 10; ++i) accept = next.errors[i] >= s_status.errors[i];

    bool finished = accept && s_training_started && !next.running &&
                    (!s_status.confirmed || s_requested_running);
    bool changed = accept && s_training_started &&
                   (finished || !s_status.confirmed || next.total != s_status.total ||
                    next.incomplete != s_status.incomplete);
    bool first_sync = false;
    if (accept) {

        first_sync = !s_training_started && !s_status.confirmed;
        s_requested_running = next.running;
        s_status = next;
        s_last_rx = esp_timer_get_time();
        g_k230_action = next.action;
        g_k230_total = next.running ? next.total : 0;
        g_k230_standard = next.running ? next.standard : 0;
        g_k230_nonstandard = next.running ? next.nonstandard : 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (first_sync && next.session[0]) {
        ESP_LOGI(TAG, "K230 synced to boot session %s (idle)", next.session);
    }
    if (!accept) {

        static int64_t last_rejected;
        int64_t now = esp_timer_get_time();
        if (!last_rejected || now - last_rejected >= 1000000) {
            k230_status_t expected;
            k230_get_status(&expected);
            ESP_LOGW(TAG, "Status not synchronized: received session=%s running=%d seq=%d; "
                     "expected session=%s running=%d seq>%d. Check K230 program/control logs.",
                     next.session, next.running, next.seq, expected.session, expected.running, expected.seq);
            last_rejected = now;
        }
    }
    if (finished) web_server_record_training(&next);
    if (finished && next.idle_timeout) ESP_LOGI(TAG, "K230 idle timeout: training ended, final report retained");
    if (finished && next.user_stop) ESP_LOGI(TAG, "K230 touch stop: training ended, final report retained");
    if (changed) {
        ESP_LOGI(TAG, "REHAB session=%s running=%d total=%d standard=%d nonstandard=%d "
                 "incomplete=%d interrupted=%d score=%d feedback=%d",
                 next.session, next.running, next.total, next.standard, next.nonstandard,
                 next.incomplete, next.interrupted, next.score, next.feedback);
        ESP_LOGI(TAG, "ERRORS left=%d right=%d level=%d bent=%d too_high=%d closed=%d short_hold=%d",
                 next.errors[1], next.errors[2], next.errors[3], next.errors[4],
                 next.errors[8], next.errors[9], next.short_hold);
    }
}

static bool send_control(int sock)
{
    char line[160];
    k230_status_t status;
    bool running;
    taskENTER_CRITICAL(&s_lock);
    status = s_status;
    running = s_requested_running;
    taskEXIT_CRITICAL(&s_lock);
    int len = status.session[0] ? snprintf(line, sizeof(line),
        "{\"type\":\"control\",\"action\":\"%s\",\"session\":\"%s\",\"running\":%s}\n",
        action_name(status.action), status.session, running ? "true" : "false") :
        snprintf(line, sizeof(line), "{\"type\":\"ping\"}\n");
    int offset = 0;
    while (offset < len) {
        int n = send(sock, line + offset, len - offset, 0);
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}

static void tcp_server_task(void *arg)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(9999),
                                  .sin_addr.s_addr = htonl(INADDR_ANY) };
    int reuse = 1;
    if (server >= 0) setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, 1) < 0) {
        ESP_LOGE(TAG, "TCP server initialization failed");
        if (server >= 0) close(server);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP listening: 192.168.4.1:9999");
    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        ESP_LOGI(TAG, "K230 connected");
        char rx[256], line[1024];
        size_t used = 0;
        bool discard = false;
        int64_t last_control = 0, last_data = esp_timer_get_time();
        char sent_session[sizeof(s_status.session)] = {0};
        bool sent_running = false;
        while (1) {
            int64_t now = esp_timer_get_time();
            k230_status_t status;
            bool requested_running;
            taskENTER_CRITICAL(&s_lock);
            status = s_status;
            requested_running = s_requested_running;
            taskEXIT_CRITICAL(&s_lock);

            if (now - last_control >= 1000000 || last_control == 0 ||
                strcmp(sent_session, status.session) || sent_running != requested_running) {
                if (!send_control(client)) break;
                last_control = now;
                strcpy(sent_session, status.session);
                sent_running = requested_running;
            }


            if (status.session[0] && now - last_data > 60000000) break;
            int n = recv(client, rx, sizeof(rx), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                break;
            }
            last_data = esp_timer_get_time();

            for (int i = 0; i < n; ++i) {
                if (rx[i] == '\n') {
                    if (!discard && used) { line[used] = '\0'; parse_status(line); }
                    used = 0;
                    discard = false;
                } else if (!discard) {
                    if (used < sizeof(line) - 1) line[used++] = rx[i];
                    else { used = 0; discard = true; ESP_LOGW(TAG, "Oversize line dropped"); }
                }
            }
        }
        close(client);
        taskENTER_CRITICAL(&s_lock);
        s_last_rx = 0;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "K230 disconnected; preserving current session");
    }
}

void k230_tcp_init(void)
{
    s_boot = esp_random();
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));


    snprintf(s_status.session, sizeof(s_status.session), "%08lx-%08lx",
             (unsigned long)s_boot, (unsigned long)s_session_number);
    s_status.action = ACTION_TRUNK_BEND;
    strcpy(s_status.state, "IDLE");
    s_requested_running = false;
    s_training_started = false;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Boot sync session=%s; K230 adopts it after reconnect", s_status.session);
    xTaskCreate(tcp_server_task, "k230_tcp", 6144, NULL, 4, NULL);
}
