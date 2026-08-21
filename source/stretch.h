#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool stretch_init(uint32_t sample_rate);
void stretch_reset(uint32_t sample_rate, float tempo_factor);
void stretch_set_tempo(float tempo_factor);
void stretch_put(const int16_t *samples, uint32_t frames);
uint32_t stretch_receive(int16_t *samples, uint32_t max_frames);
void stretch_exit(void);

#ifdef __cplusplus
}
#endif
