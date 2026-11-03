#include "espnow_comm.h"
#include "uart_startup.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "ESPNOW";


const uint8_t WATCH_MAC[6] = {0x68, 0xEE, 0x8F, 0x4D, 0x92, 0xC8};


_Static_assert(sizeof(espnow_data_t) == 14, "Unexpected ESP-NOW packet size");
_Static_assert(sizeof(espnow_time_t) == 9, "Unexpected ESP-NOW time packet size");


volatile int  g_watch_hr         = 0;
volatile int  g_watch_spo2       = 0;
volatile bool g_watch_data_valid = false;


static bool s_initialized = false;
static uint32_t s_tx_seq      = 0;
static uint32_t s_last_rx_seq = 0;
static volatile bool s_restart_requested = false;


static int s_last_action      = -1;
static int s_last_qualified   = -1;
static int s_last_unqualified = -1;
static int s_last_total       = -1;


static TickType_t s_last_rx_tick = 0;


static int s_send_fail_count = 0;


static volatile bool s_resend_exercise = false;
static volatile int  s_resend_budget = 0;
static TickType_t    s_last_resend_tick = 0;

static esp_err_t espnow_driver_start(void);


static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_send_fail_count++;
        if (s_resend_budget < 3) {
            s_resend_budget++;
            s_resend_exercise = true;
        }

        if (s_send_fail_count == 1 || (s_send_fail_count % 10) == 0) {
            ESP_LOGW(TAG, "Send FAILED (cnt=%d) — no ACK, resend budget=%d",
                     s_send_fail_count, s_resend_budget);
        }
    } else {
        s_send_fail_count = 0;
        s_resend_budget = 0;
    }
}


static void on_data_recv(const esp_now_recv_info_t *recv_info,
                         const uint8_t *data, int len)
{

    if (len != (int)sizeof(espnow_data_t)) {
        ESP_LOGW(TAG, "RX bad len=%d (expect %d)", len, (int)sizeof(espnow_data_t));
        return;
    }

    espnow_data_t pkt;
    memcpy(&pkt, data, sizeof(pkt));


    s_last_rx_tick = xTaskGetTickCount();


    if (pkt.seq != 0 && pkt.seq == s_last_rx_seq) {
        return;
    }
    s_last_rx_seq = pkt.seq;

    switch (pkt.type) {
    case PKT_HEALTH_DATA: {
        static int last_hr = -1, last_spo2 = -1;
        int hr = pkt.hr, spo2 = pkt.spo2;
        g_watch_hr   = hr;
        g_watch_spo2 = spo2;
        g_watch_data_valid = (hr > 0 || spo2 > 0);
        if (hr != last_hr || spo2 != last_spo2) {
            ESP_LOGI(TAG, "Watch: HR=%d SpO2=%d%%", hr, spo2);
            last_hr = hr; last_spo2 = spo2;
        }
        break;
    }
    case PKT_HEARTBEAT:

        ESP_LOGI(TAG, "Watch heartbeat (ESP-NOW link alive, no sensor data)");
        g_watch_hr   = 0;
        g_watch_spo2 = 0;
        g_watch_data_valid = false;
        break;
    case PKT_ACK:
        ESP_LOGD(TAG, "Watch ACK seq=%lu", (unsigned long)pkt.seq);
        break;
    case PKT_EXERCISE_DATA:
        ESP_LOGW(TAG, "Unexpected EXERCISE packet from watch");
        break;
    default:
        ESP_LOGW(TAG, "Unknown packet type=%u", pkt.type);
        break;
    }
}


static esp_err_t add_peer(void)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, WATCH_MAC, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_AP;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Add peer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}


static void send_time_packet(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) return;

    espnow_time_t pkt = {0};
    pkt.type  = PKT_TIME_SYNC;
    pkt.epoch = (uint32_t)now;
    pkt.seq   = ++s_tx_seq;

    esp_err_t ret = esp_now_send(WATCH_MAC, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Time sync send failed: %s", esp_err_to_name(ret));
    }
}


static void espnow_send_task(void *arg)
{
    ESP_LOGI(TAG, "TX task started (200ms)");
    int alive_counter = 0;
    int time_counter = 24;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        alive_counter++;


        if (s_restart_requested) {
            s_restart_requested = false;
            ESP_LOGI(TAG, "Restarting ESP-NOW to sync with new WiFi channel...");
            s_initialized = false;
            esp_now_del_peer(WATCH_MAC);
            esp_now_unregister_send_cb();
            esp_now_unregister_recv_cb();
            esp_now_deinit();
            vTaskDelay(pdMS_TO_TICKS(50));
            if (espnow_driver_start() == ESP_OK) {
                s_send_fail_count = 0;
                ESP_LOGI(TAG, "ESP-NOW restarted OK (home channel synced)");
            } else {
                ESP_LOGE(TAG, "ESP-NOW restart failed");
            }
        }


        if (g_watch_data_valid &&
            (xTaskGetTickCount() - s_last_rx_tick) > pdMS_TO_TICKS(10000)) {
            g_watch_data_valid = false;
            g_watch_hr   = 0;
            g_watch_spo2 = 0;
            ESP_LOGW(TAG, "Watch data stale (no RX 10s) — cleared HR/SpO2");
        }


        if (alive_counter % 150 == 0) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "OK uptime=%ds ch=%d RSSI=%d",
                         alive_counter / 5, ap_info.primary, ap_info.rssi);
            } else {
                ESP_LOGW(TAG, "STA not connected! uptime=%ds", alive_counter / 5);
            }


            if (!esp_now_is_peer_exist(WATCH_MAC) || s_send_fail_count >= 5) {
                ESP_LOGW(TAG, "Re-adding peer (fail_count=%d)...", s_send_fail_count);
                esp_now_del_peer(WATCH_MAC);
                add_peer();
                s_send_fail_count = 0;
            }
        }


        if (alive_counter % 150 == 0) {
            espnow_data_t pkt = {0};
            pkt.type = PKT_HEARTBEAT;
            pkt.seq  = ++s_tx_seq;
            esp_err_t ret = esp_now_send(WATCH_MAC, (uint8_t *)&pkt, sizeof(pkt));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Heartbeat fail: %s", esp_err_to_name(ret));
            }
        }


        if (++time_counter >= 25) {
            time_counter = 0;
            send_time_packet();
        }


        if (s_resend_exercise && s_last_rx_tick &&
            (xTaskGetTickCount() - s_last_rx_tick) < pdMS_TO_TICKS(10000) &&
            (xTaskGetTickCount() - s_last_resend_tick) >= pdMS_TO_TICKS(500)) {
            s_resend_exercise = false;
            s_last_resend_tick = xTaskGetTickCount();
            espnow_comm_send_exercise(g_k230_action, g_k230_standard,
                                      g_k230_nonstandard, g_k230_total);
            ESP_LOGD(TAG, "Resent latest exercise state after ACK loss");
        }

        int action      = g_k230_action;
        int qualified   = g_k230_standard;
        int unqualified = g_k230_nonstandard;
        int total       = g_k230_total;


        if (action      == s_last_action      &&
            qualified   == s_last_qualified   &&
            unqualified == s_last_unqualified &&
            total       == s_last_total) {
            continue;
        }
        s_last_action      = action;
        s_last_qualified   = qualified;
        s_last_unqualified = unqualified;
        s_last_total       = total;

        espnow_comm_send_exercise(action, qualified, unqualified, total);
    }
}


static esp_err_t espnow_driver_start(void)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_now_register_recv_cb(on_data_recv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register recv cb failed: %s", esp_err_to_name(ret));
        esp_now_deinit();
        return ret;
    }

    ret = esp_now_register_send_cb(on_data_sent);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register send cb failed: %s", esp_err_to_name(ret));
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return ret;
    }


    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, WATCH_MAC, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_AP;
    peer.encrypt = false;
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Add peer failed: %s", esp_err_to_name(ret));
        esp_now_unregister_send_cb();
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "Peer added: %02x:%02x:%02x:%02x:%02x:%02x",
                 WATCH_MAC[0], WATCH_MAC[1], WATCH_MAC[2],
                 WATCH_MAC[3], WATCH_MAC[4], WATCH_MAC[5]);

    s_initialized = true;
    return ESP_OK;
}

void espnow_comm_init(void)
{
    ESP_LOGI(TAG, "==============================================");


    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "Host STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    ESP_LOGI(TAG, "Host AP  MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Watch   MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             WATCH_MAC[0], WATCH_MAC[1], WATCH_MAC[2],
             WATCH_MAC[3], WATCH_MAC[4], WATCH_MAC[5]);
    ESP_LOGI(TAG, "==============================================");

    if (espnow_driver_start() != ESP_OK) {
        return;
    }


    xTaskCreate(espnow_send_task, "espnow_tx", 3584, NULL, 3, NULL);

    ESP_LOGI(TAG, "ESP-NOW init done.");
}


void espnow_comm_restart(void)
{
    s_restart_requested = true;
}

bool espnow_comm_watch_alive(void)
{


    if (s_last_rx_tick == 0) return false;
    return (xTaskGetTickCount() - s_last_rx_tick) <= pdMS_TO_TICKS(60000);
}

esp_err_t espnow_comm_send_exercise(int action, int qualified, int unqualified, int total)
{
    espnow_data_t pkt = {0};
    pkt.type        = PKT_EXERCISE_DATA;
    pkt.action      = (uint8_t)action;
    pkt.qualified   = (uint16_t)qualified;
    pkt.unqualified = (uint16_t)unqualified;
    pkt.total       = (uint16_t)total;
    pkt.seq         = ++s_tx_seq;

    esp_err_t ret = esp_now_send(WATCH_MAC, (uint8_t *)&pkt, sizeof(pkt));
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "TX exercise: action=%d OK=%d NG=%d total=%d",
                 action, qualified, unqualified, total);
    } else {
        ESP_LOGW(TAG, "TX fail: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_ESPNOW_NOT_FOUND) {
            esp_now_del_peer(WATCH_MAC);
            add_peer();
        }
    }
    return ret;
}
