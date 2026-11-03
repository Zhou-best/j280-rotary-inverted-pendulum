#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "uart_startup.h"
#include "espnow_comm.h"

static const char *TAG = "UART_K230";


#define UART_PORT           UART_NUM_1
#define UART_TX_PIN         18
#define UART_RX_PIN         17
#define UART_BUF_SIZE       512


int g_k230_action = 0;
int g_k230_total = 0;
int g_k230_standard = 0;
int g_k230_nonstandard = 0;


static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 57600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2,
                                        0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART1 initialized (TX:%d, RX:%d)", UART_TX_PIN, UART_RX_PIN);
}


static int parse_simple_json(const char *json_str, char *action,
                             int *total, int *standard, int *nonstandard)
{
    char *p;


    p = strstr(json_str, "\"action\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            if (*p == '\"') p++;
            int i = 0;
            while (*p && *p != '\"' && *p != ',' && *p != '}' && i < 31) {
                action[i++] = *p++;
            }
            action[i] = '\0';
        } else return -1;
    } else return -1;


    p = strstr(json_str, "\"total\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            *total = atoi(p);
        }
    } else *total = 0;


    p = strstr(json_str, "\"standard\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            *standard = atoi(p);
        }
    } else *standard = 0;


    p = strstr(json_str, "\"nonstandard\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            *nonstandard = atoi(p);
        }
    } else *nonstandard = 0;

    return 0;
}


static void uart_receive_task(void *pvParameters)
{
    uint8_t data[UART_BUF_SIZE];
    char action[32] = {0};


    int total = 0, standard = 0, nonstandard = 0;
    static char line_buf[UART_BUF_SIZE];
    static int line_pos = 0;
    int idle_ticks = 0;

    ESP_LOGI(TAG, "RX task running, waiting for K230 data on GPIO%d...", UART_RX_PIN);

    while (1) {
        int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        if (len > 0) {
            idle_ticks = 0;
            for (int i = 0; i < len; i++) {
                char c = data[i];
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';

                        if (parse_simple_json(line_buf, action,
                                              &total, &standard, &nonstandard) == 0) {

                            static int last_t = -1, last_s = -1, last_n = -1;
                            if (total != last_t || standard != last_s || nonstandard != last_n) {
                                ESP_LOGI(TAG, "K230: %s total=%d std=%d nonstd=%d",
                                         action, total, standard, nonstandard);
                                last_t = total; last_s = standard; last_n = nonstandard;
                            }


                            if (strcmp(action, "expand_chest") == 0) {
                                g_k230_action = ACTION_EXPAND_CHEST;
                            } else if (strcmp(action, "push_up") == 0) {
                                g_k230_action = ACTION_PUSH_UP;
                            } else if (strcmp(action, "elbow_curl") == 0) {
                                g_k230_action = ACTION_ELBOW_CURL;
                            } else {
                                g_k230_action = 0;
                            }


                            g_k230_total = total;
                            g_k230_standard = standard;
                            g_k230_nonstandard = nonstandard;
                        }
                        line_pos = 0;
                    }
                } else {
                    if (line_pos < UART_BUF_SIZE - 1) {
                        line_buf[line_pos++] = c;
                    }
                }
            }
        } else {
            idle_ticks++;

            if (idle_ticks % 600 == 0) {
                ESP_LOGI(TAG, "K230 idle %d s", idle_ticks / 10);
            }
        }
    }
}


void uart_startup(void)
{
    uart_init();
    xTaskCreate(uart_receive_task, "uart_rx", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART receive task started");
}


void uart_send_to_k230(const char *data, size_t len)
{
    int sent = uart_write_bytes(UART_PORT, data, len);
    if (sent < 0) {
        ESP_LOGE(TAG, "TX to K230 failed");
    } else {
        ESP_LOGI(TAG, "TX to K230: %.*s", (int)len, data);
    }
}
