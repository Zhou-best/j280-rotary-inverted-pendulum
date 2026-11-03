#include "fpga_link.h"
#include "local_tts.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define FPGA_PULSE_GPIO     18
#define FPGA_ACK_GPIO       17
#define PULSE_UNIT_MS       100
#define FPGA_SINE_BUTTON_GPIO 0
#define FPGA_LINK_TTS 0

static const char *TAG = "FPGA_LINK";
static QueueHandle_t s_pulse_q = NULL;
static uint8_t s_cur_mode = 0;

static const char *mode_name_cn(uint8_t m)
{
    switch (m) {
        case 0:  return "零度";
        case 1:  return "正四十五度";
        case 2:  return "负四十五度";
        case 3:  return "正九十度";
        case 4:  return "负九十度";
        default: return "目标角度";
    }
}


static void fpga_pulse_task(void *arg)
{
    (void)arg;
    uint8_t mode;

    while (1) {
        if (xQueueReceive(s_pulse_q, &mode, portMAX_DELAY) == pdTRUE) {
            int width_ms = ((int)mode + 1) * PULSE_UNIT_MS;
            gpio_set_level(FPGA_PULSE_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(width_ms));
            gpio_set_level(FPGA_PULSE_GPIO, 0);
            ESP_LOGI(TAG, "PULSE mode=%u width=%dms (GPIO%d -> AB11)",
                     (unsigned)mode, width_ms, FPGA_PULSE_GPIO);
        }
    }
}


void fpga_link_send_mode(uint8_t mode)
{
    if (mode > 5) {
        ESP_LOGW(TAG, "mode %u out of range, ignored", (unsigned)mode);
        return;
    }
    if (s_pulse_q == NULL) {
        ESP_LOGW(TAG, "pulse queue not ready, mode %u dropped", (unsigned)mode);
        return;
    }
    if (xQueueSend(s_pulse_q, &mode, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "pulse queue full, mode %u dropped", (unsigned)mode);
    }
}

void fpga_link_send_stop(void)
{
    fpga_link_send_mode(0);
}

void fpga_link_send_next(void)
{

    ESP_LOGW(TAG, "NEXT not supported in pulse protocol");
}

void fpga_link_send_heartbeat(void)
{

}

void fpga_link_handle_gesture(const char *arg)
{
    uint8_t mode;
    if (arg == NULL) return;

    if (!strcmp(arg, "left")) {
        mode = FPGA_MODE_LEFT;
    } else if (!strcmp(arg, "right")) {
        mode = FPGA_MODE_RIGHT;
    } else if (!strcmp(arg, "stop")) {
        mode = FPGA_MODE_STOP;
    } else {
        ESP_LOGW(TAG, "unknown gesture: %s", arg);
        return;
    }

    ESP_LOGI(TAG, "gesture %s -> mode %u", arg, (unsigned)mode);


    fpga_link_send_mode(mode);

    uint8_t old_mode = s_cur_mode;
    s_cur_mode = mode;
#if FPGA_LINK_TTS
    char text[96];
    if (old_mode == mode) {
        snprintf(text, sizeof(text), "已经在%s", mode_name_cn(mode));
    } else {
        snprintf(text, sizeof(text), "正在从%s转到%s",
                 mode_name_cn(old_mode), mode_name_cn(mode));
    }
    ESP_LOGI(TAG, "TTS: %s", text);
    local_tts_play(text);
#else
    (void)old_mode;
#endif
}


static void fpga_ack_task(void *arg)
{
    (void)arg;
    int last = 0;
    int64_t rise_us = 0;

    while (1) {
        int level = gpio_get_level(FPGA_ACK_GPIO);
        if (level && !last) {
            rise_us = esp_timer_get_time();
        } else if (!level && last) {
            int64_t width_us = esp_timer_get_time() - rise_us;
            int mode = (int)((width_us + 50000) / 100000) - 1;
            if (mode >= 0 && mode <= 5) {
                ESP_LOGI(TAG, "FPGA ACK mode=%d (width=%lld ms)",
                         mode, (long long)(width_us / 1000));
            } else {
                ESP_LOGW(TAG, "FPGA ACK width=%lld us (unexpected)",
                         (long long)width_us);
            }
        }
        last = level;


        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


static void fpga_button_task(void *arg)
{
    (void)arg;
    int last = 1;
    while (1) {
        int level = gpio_get_level(FPGA_SINE_BUTTON_GPIO);
        if (last == 1 && level == 0) {
            ESP_LOGI(TAG, "BOOT button -> mode 5 (sine)");
            fpga_link_send_mode(5);
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void fpga_link_init(void)
{

    gpio_reset_pin(FPGA_PULSE_GPIO);
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << FPGA_PULSE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    gpio_set_level(FPGA_PULSE_GPIO, 0);


    gpio_reset_pin(FPGA_ACK_GPIO);
    gpio_config_t ack_cfg = {
        .pin_bit_mask = (1ULL << FPGA_ACK_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ack_cfg));


    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << FPGA_SINE_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    s_pulse_q = xQueueCreate(8, sizeof(uint8_t));


    xTaskCreate(fpga_pulse_task, "fpga_pulse", 3072, NULL, 6, NULL);
    xTaskCreate(fpga_ack_task,   "fpga_ack",   2048, NULL, 3, NULL);
    xTaskCreate(fpga_button_task, "fpga_btn",  2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "PULSE protocol ready: GPIO%d -> AB11, ACK GPIO=%d, unit=%dms",
             FPGA_PULSE_GPIO, FPGA_ACK_GPIO, PULSE_UNIT_MS);
}


static void fpga_selftest_task(void *arg)
{
    (void)arg;
    const uint8_t seq[] = {0, FPGA_MODE_RIGHT, FPGA_MODE_LEFT, 0};
    int i = 0;
    while (1) {
        ESP_LOGI(TAG, "selftest -> mode %u", (unsigned)seq[i]);
        fpga_link_send_mode(seq[i]);
        i = (i + 1) % (sizeof(seq) / sizeof(seq[0]));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void fpga_link_selftest_start(void)
{
    xTaskCreate(fpga_selftest_task, "fpga_test", 4096, NULL, 3, NULL);
}
