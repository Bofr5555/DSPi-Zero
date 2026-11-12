#pragma once

#include "pico.h"

// RP2350 GPIO API. The Pi's pins are driven through the broadcom/gpio.h helpers instead, so these
// only remember the last function and direction set per pin, for code that reads them back.
typedef enum gpio_function {
    GPIO_FUNC_HSTX = 0,
    GPIO_FUNC_SPI = 1,
    GPIO_FUNC_UART = 2,
    GPIO_FUNC_I2C = 3,
    GPIO_FUNC_PWM = 4,
    GPIO_FUNC_SIO = 5,
    GPIO_FUNC_PIO0 = 6,
    GPIO_FUNC_PIO1 = 7,
    GPIO_FUNC_PIO2 = 8,
    GPIO_FUNC_GPCK = 9,
    GPIO_FUNC_USB = 10,
    GPIO_FUNC_UART_AUX = 11,
    GPIO_FUNC_NULL = 0x1f,
} gpio_function_t;

// The Broadcom GPIO helpers own these two names, so the SDK versions are renamed at the source.
#define gpio_set_function pico_gpio_set_function
#define gpio_get_function pico_gpio_get_function

#define GPIO_OUT 1
#define GPIO_IN  0
#define NUM_BANK0_GPIOS 48u

enum gpio_irq_level {
    GPIO_IRQ_LEVEL_LOW = 0x1u,
    GPIO_IRQ_LEVEL_HIGH = 0x2u,
    GPIO_IRQ_EDGE_FALL = 0x4u,
    GPIO_IRQ_EDGE_RISE = 0x8u,
};

// IO bank 0 edge interrupts (Control Surfaces IR receiver). Enabling records nothing and no edge is
// ever reported.
#define IO_IRQ_BANK0 21u
void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled);
void gpio_acknowledge_irq(uint gpio, uint32_t event_mask);

void gpio_init(uint gpio);
void gpio_deinit(uint gpio);
void gpio_set_function(uint gpio, gpio_function_t fn);
gpio_function_t gpio_get_function(uint gpio);
void gpio_set_dir(uint gpio, bool out);
bool gpio_get_dir(uint gpio);
void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
void gpio_pull_up(uint gpio);
void gpio_pull_down(uint gpio);
void gpio_disable_pulls(uint gpio);
void gpio_set_input_enabled(uint gpio, bool enabled);
