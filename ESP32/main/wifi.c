#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "lwip/ip4_addr.h"
#include "wifi.h"
#include "espnow_comm.h"

static const char *TAG = "WIFI";


#define ROUTER_SSID     "WORLD NATURE"
#define ROUTER_PASS     "18220465159zpzp&"


#define NVS_NS_WIFICFG  "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"


#define ROUTER_FORCE_DEFAULT  1

static bool s_wifi_initialized = false;
static int s_sta_retry = 0;


static bool s_wifi_connected = false;
static TaskHandle_t s_reconnect_task = NULL;
static SemaphoreHandle_t s_sta_lock = NULL;
static void sta_reconnect_after(uint32_t delay_ms);


static char s_router_ssid[33] = ROUTER_SSID;
static char s_router_pass[65] = ROUTER_PASS;
static char s_sta_ip[16] = {0};
static volatile uint8_t s_last_reason = 0;


static volatile bool s_reconfiguring = false;


static volatile bool s_diag_pending = true;

bool is_wifi_connected(void)
{
    return s_wifi_connected;
}

void wifi_get_router_credentials(char *ssid_out, size_t ssid_len,
                                 char *pass_out, size_t pass_len)
{
    if (ssid_out && ssid_len) {
        strncpy(ssid_out, s_router_ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }
    if (pass_out && pass_len) {
        strncpy(pass_out, s_router_pass, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    }
}

void wifi_get_sta_ip(char *buf, size_t len)
{
    if (buf && len) {
        strncpy(buf, s_sta_ip[0] ? s_sta_ip : "-", len - 1);
        buf[len - 1] = '\0';
    }
}

uint8_t wifi_get_last_reason(void) { return s_last_reason; }


const char *wifi_auth_mode_str(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:            return "开放(无密码)";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2混合";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2企业";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3混合";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "其他";
    }
}


static void router_cfg_load(void)
{
#if ROUTER_FORCE_DEFAULT
    ESP_LOGW(TAG, "Temporary hotspot override: using compiled \"%s\" (NVS ignored)",
             s_router_ssid);
    return;
#endif
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFICFG, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved router config — using compiled default \"%s\"",
                 s_router_ssid);
        return;
    }

    size_t len = sizeof(s_router_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_router_ssid, &len) == ESP_OK
        && s_router_ssid[0] != '\0') {
        len = sizeof(s_router_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, s_router_pass, &len) != ESP_OK)
            s_router_pass[0] = '\0';
        ESP_LOGI(TAG, "Saved router config loaded: \"%s\"", s_router_ssid);
    } else {

        strncpy(s_router_ssid, ROUTER_SSID, sizeof(s_router_ssid) - 1);
        strncpy(s_router_pass, ROUTER_PASS, sizeof(s_router_pass) - 1);
        ESP_LOGI(TAG, "Saved SSID unreadable — using compiled default \"%s\"",
                 s_router_ssid);
    }
    nvs_close(h);
}


esp_err_t wifi_set_router_credentials(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0')  return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) > 32)         return ESP_ERR_INVALID_ARG;
    size_t pass_len = pass ? strlen(pass) : 0;
    if (pass_len > 64 || (pass_len > 0 && pass_len < 8)) return ESP_ERR_INVALID_ARG;
    if (pass_len == 64 && strspn(pass, "0123456789abcdefABCDEF") != 64)
        return ESP_ERR_INVALID_ARG;
    if (!s_sta_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sta_lock, portMAX_DELAY);

    strncpy(s_router_ssid, ssid, sizeof(s_router_ssid) - 1);
    s_router_ssid[sizeof(s_router_ssid) - 1] = '\0';
    strncpy(s_router_pass, pass ? pass : "", sizeof(s_router_pass) - 1);
    s_router_pass[sizeof(s_router_pass) - 1] = '\0';


    nvs_handle_t h;
    esp_err_t nvs_ret = nvs_open(NVS_NS_WIFICFG, NVS_READWRITE, &h);
    if (nvs_ret == ESP_OK) {
        nvs_ret = nvs_set_str(h, NVS_KEY_SSID, s_router_ssid);
        if (nvs_ret == ESP_OK) nvs_ret = nvs_set_str(h, NVS_KEY_PASS, s_router_pass);
        if (nvs_ret == ESP_OK) nvs_ret = nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open failed: %s — config will NOT survive reboot",
                 esp_err_to_name(nvs_ret));
    }


    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, s_router_ssid, strlen(s_router_ssid));
    memcpy(sta.sta.password, s_router_pass, strlen(s_router_pass));
    sta.sta.pmf_cfg.capable  = true;
    sta.sta.pmf_cfg.required = false;

    s_reconfiguring = true;
    s_wifi_connected = false;
    s_sta_ip[0] = '\0';
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_reconfiguring = false;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_sta_lock);
        return ret;
    }
    s_sta_retry = 0;
    s_last_reason = 0;
    s_diag_pending = true;
    ESP_LOGI(TAG, "New router config applied: \"%s\" — reconnecting", s_router_ssid);
    sta_reconnect_after(0);
    xSemaphoreGive(s_sta_lock);
    if (nvs_ret != ESP_OK)
        ESP_LOGE(TAG, "Router config not persisted: %s", esp_err_to_name(nvs_ret));
    return nvs_ret;
}


static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
        case 1:  return "UNSPECIFIED";
        case 2:  return "AUTH_EXPIRE";
        case 3:  return "AUTH_LEAVE";
        case 4:  return "ASSOC_EXPIRE";
        case 5:  return "ASSOC_TOOMANY";
        case 6:  return "NOT_AUTHED";
        case 7:  return "NOT_ASSOCED";
        case 8:  return "ASSOC_LEAVE";
        case 9:  return "ASSOC_NOT_AUTHED";
        case 10: return "DISASSOC_PWRCAP_BAD";
        case 11: return "DISASSOC_SUPCHAN_BAD";
        case 13: return "IE_INVALID";
        case 14: return "MIC_FAILURE";
        case 15: return "4WAY_HANDSHAKE_TIMEOUT";
        case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
        case 17: return "IE_IN_4WAY_DIFFERS";
        case 18: return "GROUP_CIPHER_INVALID";
        case 19: return "PAIRWISE_CIPHER_INVALID";
        case 20: return "AKMP_INVALID";
        case 21: return "UNSUPP_RSN_IE_VERSION";
        case 22: return "INVALID_RSN_IE_CAP";
        case 23: return "802_1X_AUTH_FAILED";
        case 24: return "CIPHER_SUITE_REJECTED";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";


        case 210: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY (加密方式不兼容)";
        case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD (低于加密阈值)";
        case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD (信号太弱)";
        default:  return "UNKNOWN";
    }
}

const char *wifi_get_last_reason_str(void)
{
    return s_last_reason ? disconnect_reason_str(s_last_reason) : "-";
}


static void sta_diag_scan(void)
{
    s_diag_pending = false;

    uint8_t ssid[33] = {0};
    strncpy((char *)ssid, s_router_ssid, sizeof(ssid) - 1);

    wifi_scan_config_t sc = {
        .ssid        = ssid,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t ret = esp_wifi_scan_start(&sc, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "诊断扫描未能启动: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGW(TAG, "诊断：扫不到 \"%s\" —— 没有这个热点，或不在 2.4GHz / 太远",
                 s_router_ssid);
        return;
    }
    if (n > 16) n = 16;

    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    if (!rec) { esp_wifi_clear_ap_list(); return; }
    esp_wifi_scan_get_ap_records(&n, rec);

    const size_t pass_len = strlen(s_router_pass);

    for (int i = 0; i < n; i++) {
        ESP_LOGW(TAG, "诊断 \"%s\" 加密=%s(%d) 信道=%d 信号=%d dBm",
                 (const char *)rec[i].ssid,
                 wifi_auth_mode_str(rec[i].authmode),
                 (int)rec[i].authmode, rec[i].primary, rec[i].rssi);
        if (strcmp((char *)rec[i].ssid, s_router_ssid) != 0) continue;


        if (rec[i].authmode == WIFI_AUTH_OPEN && pass_len >= 8) {
            ESP_LOGE(TAG, "→ 热点\"%s\"没密码，但网页里存了 %u 位密码。IDF 见"
                          "密码 ≥8 位就把加密阈值自动抬到 WPA2，于是把开放热点"
                          "拒了。把网页密码框清空、保存重连即可。",
                     (const char *)rec[i].ssid, (unsigned)pass_len);
        } else if (rec[i].authmode == WIFI_AUTH_OPEN) {
            ESP_LOGW(TAG, "→ 热点无密码、本机也无密码，本应能连上——"
                          "请把这段日志发我。");
        } else if (pass_len == 0) {
            ESP_LOGE(TAG, "→ 热点是有密码的（%s），但网页密码框是空的。"
                          "请填入它的密码。", wifi_auth_mode_str(rec[i].authmode));
        } else if (rec[i].authmode == WIFI_AUTH_WPA3_PSK ||
                   rec[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK) {
            ESP_LOGW(TAG, "→ 热点是 WPA3 / WPA2+WPA3 混合模式。若仍连不上，"
                          "把手机热点的「安全性」改成 WPA2-Personal。");
        } else {
            ESP_LOGW(TAG, "→ 加密方式是 %s，与 %u 位密码本应匹配。若仍 210，"
                          "请把日志发我。",
                     wifi_auth_mode_str(rec[i].authmode), (unsigned)pass_len);
        }
    }

    free(rec);
    esp_wifi_clear_ap_list();
}


static void sta_reconnect_task(void *arg)
{
    uint32_t delay_ms;
    while (1) {
        xTaskNotifyWait(0, UINT32_MAX, &delay_ms, portMAX_DELAY);

        while (xTaskNotifyWait(0, UINT32_MAX, &delay_ms, pdMS_TO_TICKS(delay_ms)) == pdTRUE) {}
        xSemaphoreTake(s_sta_lock, portMAX_DELAY);
        if (!s_wifi_connected && !s_reconfiguring) {
            if (s_diag_pending) sta_diag_scan();
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "STA connect failed: %s; retry in 15s", esp_err_to_name(ret));
                sta_reconnect_after(15000);
            }
        }
        xSemaphoreGive(s_sta_lock);
    }
}

static void sta_reconnect_after(uint32_t delay_ms)
{
    xTaskNotify(s_reconnect_task, delay_ms, eSetValueWithOverwrite);
}


static bool s_sntp_started = false;

static void sntp_sync_notification(struct timeval *tv)
{
    struct tm ti;
    localtime_r(&tv->tv_sec, &ti);
    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

static void sntp_start_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started: ntp.aliyun.com");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started, SSID: %s", AP_SSID);
        ESP_LOGI(TAG, "Password: %s", AP_PASS);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting to router: %s", s_router_ssid);
        sta_reconnect_after(0);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected! Reason: %d (%s)",
                 evt->reason, disconnect_reason_str(evt->reason));

        s_wifi_connected = false;
        s_last_reason = evt->reason;
        s_sta_ip[0] = '\0';

        if (s_reconfiguring) {
            ESP_LOGW(TAG, "Disconnect is from reconfiguring — skip auto-retry");
            return;
        }


        if (evt->reason == 202 || evt->reason == 201 || evt->reason == 210 ||
            evt->reason == 211 || evt->reason == 212) {
            ESP_LOGW(TAG, "Router \"%s\" rejected (%s) — retry in 15s",
                     s_router_ssid, disconnect_reason_str(evt->reason));
            sta_reconnect_after(15000);
            return;
        }

        if (s_sta_retry < 10) s_sta_retry++;
        uint32_t retry_ms = s_sta_retry < 10 ? 3000 : 15000;
        ESP_LOGW(TAG, "Router disconnected; retry in %u ms", (unsigned)retry_ms);
        sta_reconnect_after(retry_ms);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Router IP: " IPSTR "; K230 uses SoftAP 192.168.4.1", IP2STR(&event->ip_info.ip));
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_retry = 0;
        s_last_reason = 0;
        s_wifi_connected = true;

        espnow_comm_restart();

        sntp_start_once();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_wifi_connected = false;
        s_sta_ip[0] = '\0';
        sta_reconnect_after(15000);
    }
}


void wifi_init_softap(void)
{
    if (s_wifi_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return;
    }
    s_sta_lock = xSemaphoreCreateMutex();
    if (!s_sta_lock || xTaskCreate(sta_reconnect_task, "sta_reconn", 4096,
                                  NULL, 4, &s_reconnect_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot initialize STA reconnect worker");
        if (s_sta_lock) vSemaphoreDelete(s_sta_lock);
        s_sta_lock = NULL;
        return;
    }
    s_wifi_initialized = true;

    router_cfg_load();

    ESP_LOGI(TAG, "Initializing WiFi AP+STA...");

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    esp_err_t dhcp_ret = esp_netif_dhcps_stop(ap_netif);
    if (dhcp_ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ESP_ERROR_CHECK(dhcp_ret);
    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = true;


    cfg.dynamic_rx_buf_num = 10;
    cfg.dynamic_tx_buf_num = 10;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));


    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .channel = 11,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));


    wifi_config_t sta_config = {0};
    memcpy(sta_config.sta.ssid, s_router_ssid, strlen(s_router_ssid));
    memcpy(sta_config.sta.password, s_router_pass, strlen(s_router_pass));


    sta_config.sta.pmf_cfg.capable  = true;
    sta_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    ESP_ERROR_CHECK(esp_wifi_start());


    esp_err_t txp = esp_wifi_set_max_tx_power(40);
    if (txp == ESP_OK) {
        ESP_LOGI(TAG, "WiFi max TX power: 10 dBm (screen EMI mitigation)");
    } else {
        ESP_LOGW(TAG, "Set WiFi TX power failed: %s", esp_err_to_name(txp));
    }


    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi PS: NONE (power save disabled, ESP-NOW ready)");
}
