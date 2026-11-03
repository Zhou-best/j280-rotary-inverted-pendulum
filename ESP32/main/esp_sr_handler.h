#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    VOICE_CMD_NONE                            = -1,
    VOICE_CMD_QING_JIN_RU_DOU_BAO             = 0,
    VOICE_CMD_FAN_HUI_SHANG_YI_JIE_MIAN       = 1,
    VOICE_CMD_JIN_XING_KUO_XIONG              = 2,
    VOICE_CMD_JIN_XING_FU_WO_CHENG            = 3,
    VOICE_CMD_JIN_XING_SHEN_DUN               = 4,
    VOICE_CMD_KAI_SHI_JI_SHU                  = 5,
    VOICE_CMD_QING_JIE_SHU_DANG_QIAN_YUN_DONG = 6,
    VOICE_CMD_BO_BAO_DANG_QIAN_JI_SHU         = 7,
    VOICE_CMD_SHENG_CHENG_JIAN_SHEN_BAO_GAO    = 8,
    VOICE_CMD_JIN_RU_LIAO_TIAN_DUI_HUA         = 9,
    VOICE_CMD_CHONG_SHU_JIAN_SHEN_BAO_GAO      = 10,
    VOICE_CMD_COUNT
} voice_cmd_id_t;


esp_err_t esp_sr_handler_init(void);


esp_err_t esp_sr_handler_start(QueueHandle_t cmd_queue);


esp_err_t esp_sr_handler_stop(void);


const char *esp_sr_get_cmd_text(voice_cmd_id_t cmd_id);

#ifdef __cplusplus
}
#endif
