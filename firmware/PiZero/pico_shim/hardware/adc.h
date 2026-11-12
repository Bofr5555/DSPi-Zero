#pragma once

#include "pico.h"

// No ADC on the Pi. Reads return 0, which DSPi's temperature conversion turns into a fixed value.
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
