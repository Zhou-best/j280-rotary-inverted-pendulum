#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define DOUBAO_API_KEY      "YOUR_DOUBAO_API_KEY"
#define DOUBAO_ENDPOINT_ID  "YOUR_ENDPOINT_ID"
#define DOUBAO_CHAT_URL     "https://ark.cn-beijing.volces.com/api/v3/chat/completions"


#define VOLC_TTS_APP_ID     "YOUR_APP_ID"
#define VOLC_TTS_TOKEN      "YOUR_TTS_TOKEN"
#define VOLC_TTS_API_KEY    "YOUR_TTS_API_KEY"
#define VOLC_TTS_URL        "https://openspeech.bytedance.com/api/v1/tts"


#define AI_SYSTEM_PROMPT \
    "你是一个友好、健谈的中文语音助手，名字叫小智。" \
    "你既懂康复运动（躯干侧屈、侧平举、双臂屈伸肘），也能回答日常知识、常识、地理人文和闲聊。" \
    "回答要口语化、自然连贯，适合直接朗读；每次不超过100字，最多四句话。" \
    "运动相关的问题给专业且安全的建议，不作医疗诊断；其他话题正常回答，不要拒绝或刻意回避。"


char *ai_chat_send(const char *user_message);


typedef void (*ai_sentence_cb_t)(const char *sentence, void *user);
char *ai_chat_stream(const char *user_message, ai_sentence_cb_t on_sentence, void *user);


int16_t *ai_tts_synthesize(const char *text, size_t *out_samples);


esp_err_t ai_conversation(const char *question);


char *ai_chat_text_only(const char *user_message);


char *ai_chat_text_only_with_error(const char *user_message, const char **out_error);

#ifdef __cplusplus
}
#endif
