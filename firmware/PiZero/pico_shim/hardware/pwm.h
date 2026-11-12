#pragma once

#include "pico.h"

// RP2350 PWM API, for Control Surfaces LED outputs. Slice and channel arithmetic is the SDK's;
// the settings themselves go nowhere.
#define NUM_PWM_SLICES 12u

static inline uint pwm_gpio_to_slice_num(uint gpio) {
    return gpio < 32u ? ((gpio >> 1u) & 7u) : 8u + ((gpio >> 1u) & 3u);
}
static inline uint pwm_gpio_to_channel(uint gpio) {
    return gpio & 1u;
}

void pwm_set_wrap(uint slice_num, uint16_t wrap);
void pwm_set_clkdiv(uint slice_num, float divider);
void pwm_set_gpio_level(uint gpio, uint16_t level);
void pwm_set_enabled(uint slice_num, bool enabled);
