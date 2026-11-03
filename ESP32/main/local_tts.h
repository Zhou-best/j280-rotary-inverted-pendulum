#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


esp_err_t local_tts_init(void);


esp_err_t local_tts_play(const char *chinese_text);


esp_err_t local_tts_play_wait(const char *chinese_text);

#ifdef __cplusplus
}
#endif
