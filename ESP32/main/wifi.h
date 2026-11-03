#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define AP_SSID     "ESP32-HeartRate"
#define AP_PASS     "12345678"


void wifi_init_softap(void);


bool is_wifi_connected(void);


esp_err_t wifi_set_router_credentials(const char *ssid, const char *pass);
void      wifi_get_router_credentials(char *ssid_out, size_t ssid_len,
                                      char *pass_out, size_t pass_len);
void      wifi_get_sta_ip(char *buf, size_t len);
uint8_t   wifi_get_last_reason(void);
const char *wifi_get_last_reason_str(void);

const char *wifi_auth_mode_str(wifi_auth_mode_t m);

#endif
