#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


char *ai_asr_recognize(const int16_t *pcm, size_t samples);


typedef enum {
    AI_ASR_STATUS_OK = 0,
    AI_ASR_STATUS_NO_SPEECH,
    AI_ASR_STATUS_BUSY,
    AI_ASR_STATUS_FAIL,
} ai_asr_status_t;

ai_asr_status_t ai_asr_last_status(void);

#ifdef __cplusplus
}
#endif
