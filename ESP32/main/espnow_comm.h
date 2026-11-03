#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    PKT_EXERCISE_DATA = 1,
    PKT_HEARTBEAT     = 2,
    PKT_ACK           = 3,
    PKT_HEALTH_DATA   = 4,
    PKT_TIME_SYNC     = 5,
} espnow_pkt_type_t;


typedef enum {
    ACTION_TRUNK_BEND   = 0,
    ACTION_EXPAND_CHEST = ACTION_TRUNK_BEND,
    ACTION_SIDE_RAISE   = 1,
    ACTION_PUSH_UP      = ACTION_SIDE_RAISE,
    ACTION_ELBOW_CURL   = 2,
} exercise_action_t;


typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  action;
    uint16_t qualified;
    uint16_t unqualified;
    uint16_t total;
    uint8_t  hr;
    uint8_t  spo2;
    uint32_t seq;
} espnow_data_t;


typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t epoch;
    uint32_t seq;
} espnow_time_t;


extern const uint8_t WATCH_MAC[6];


extern volatile int  g_watch_hr;
extern volatile int  g_watch_spo2;
extern volatile bool g_watch_data_valid;


void espnow_comm_init(void);


void espnow_comm_restart(void);


bool espnow_comm_watch_alive(void);


esp_err_t espnow_comm_send_exercise(int action, int qualified, int unqualified, int total);

#ifdef __cplusplus
}
#endif
