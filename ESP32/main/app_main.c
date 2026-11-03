#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "tft_spi.h"
#include "uart_startup.h"
#include "i2s_audio.h"
#include "esp_sr_handler.h"

#include "wifi.h"
#include "web_server.h"
#include "espnow_comm.h"
#include "local_tts.h"
#include "k230_tcp.h"
#include "fpga_link.h"

#define FPGA_LINK_SELFTEST 0

static const char *TAG = "MAIN";


QueueHandle_t g_voice_cmd_queue = NULL;


static void mem_monitor_task(void *pvParameters)
{
    while (1) {
        size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI("MEM", "DRAM free: %u / %u KiB (%u%% used) | "
                       "PSRAM free: %u / %u KiB (%u%% used)",
                 dram_free / 1024, dram_total / 1024,
                 (unsigned)(((dram_total - dram_free) * 100) / dram_total),
                 psram_free / 1024, psram_total / 1024,
                 psram_total > 0
                     ? (unsigned)(((psram_total - psram_free) * 100) / psram_total)
                     : 0);

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "System starting...");


    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    tft_init();
    button_init();


    ESP_ERROR_CHECK(i2s_audio_init());

    ret = local_tts_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Local speech unavailable: %s; check voice_data partition flashing", esp_err_to_name(ret));


    i2s_speaker_count_test();
    i2s_audio_start_clock_keeper();


    g_voice_cmd_queue = xQueueCreate(10, sizeof(voice_cmd_id_t));
    if (g_voice_cmd_queue) {
        esp_err_t sr_err = esp_sr_handler_init();
        if (sr_err == ESP_OK) {
            ESP_LOGI(TAG, "ESP-SR initialized, starting voice recognition...");
            esp_sr_handler_start(g_voice_cmd_queue);
        } else {
            ESP_LOGW(TAG, "ESP-SR init failed (err %s), buttons-only mode",
                     esp_err_to_name(sr_err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to create voice cmd queue");
    }


    wifi_init_softap();
    web_server_init();
    k230_tcp_init();
    fpga_link_init();
#if FPGA_LINK_SELFTEST
    fpga_link_selftest_start();
#endif
    espnow_comm_init();


    xTaskCreate(mem_monitor_task, "mem_mon", 4096, NULL, 1, NULL);


    menu_init();
    while (1) {
        menu_run();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
