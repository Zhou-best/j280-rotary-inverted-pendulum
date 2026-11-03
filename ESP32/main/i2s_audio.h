#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_AUDIO_BCLK      GPIO_NUM_1
#define I2S_AUDIO_LRCK      GPIO_NUM_2
#define I2S_AUDIO_DIN        GPIO_NUM_42
#define I2S_AUDIO_DOUT       GPIO_NUM_21
#define I2S_AUDIO_SAMPLE_RATE 16000

esp_err_t i2s_audio_init(void);
esp_err_t i2s_audio_deinit(void);


esp_err_t i2s_audio_write(const int16_t *data, size_t samples, uint32_t timeout_ms);
esp_err_t i2s_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms);
int16_t *i2s_audio_record_ms(uint32_t duration_ms, size_t *out_samples);
esp_err_t i2s_audio_play(const int16_t *data, size_t samples);


esp_err_t i2s_audio_play_rate(const int16_t *data, size_t samples, uint32_t sample_rate);


esp_err_t i2s_audio_play_multi(const int16_t *const *data, const size_t *samples,
                               size_t count, uint32_t sample_rate);
void i2s_test_tone(void);

extern volatile bool s_i2s_tx_active;
extern volatile bool s_i2s_rx_grabbed;

void i2s_audio_start_clock_keeper(void);
void i2s_audio_grab_rx(void);
void i2s_audio_release_rx(void);
size_t i2s_audio_read_chunk(int16_t *buf, size_t max_samples, uint32_t timeout_ms);
i2s_chan_handle_t i2s_audio_get_rx_channel(void);


void i2s_speaker_count_test(void);

#ifdef __cplusplus
}
#endif
