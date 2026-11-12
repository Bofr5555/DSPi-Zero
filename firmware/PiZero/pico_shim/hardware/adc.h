#pragma once

#include "pico.h"

// No ADC on the Pi. The temperature sensor input (the last one, as on the RP2350A) reads a fixed
// code for 27 C; every other input reads 0.
#define NUM_ADC_CHANNELS 5
#define ADC_TEMPERATURE_CHANNEL_NUM (NUM_ADC_CHANNELS - 1)
void adc_init(void);
void adc_gpio_init(uint gpio);
void adc_select_input(uint input);
uint adc_get_selected_input(void);
void adc_set_temp_sensor_enabled(bool enable);
uint16_t adc_read(void);
void adc_set_round_robin(uint input_mask);
void adc_fifo_setup(bool en, bool dreq_en, uint16_t dreq_thresh, bool err_in_fifo, bool byte_shift);
void adc_run(bool run);
void adc_set_clkdiv(float clkdiv);
