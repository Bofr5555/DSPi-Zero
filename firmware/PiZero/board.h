#pragma once

#include <stdint.h>

#if BCM_VERSION == 2835
// First-generation Pi Zero and Zero W.
#define BOARD_ACT_LED_PIN       47
#define BOARD_ACT_LED_ACTIVE_LOW 1
#elif BCM_VERSION == 2837
// Zero 2 W. Polarity is from the Linux device tree and not yet confirmed on hardware.
#define BOARD_ACT_LED_PIN       29
#define BOARD_ACT_LED_ACTIVE_LOW 1
#else
#error "Unsupported BCM_VERSION"
#endif

#define BOARD_UART_TX_PIN       14
#define BOARD_UART_RX_PIN       15
#define BOARD_UART_BAUD         115200
// Matches init_uart_clock in boot/config.txt, so the UART works before the first mailbox call.
#define BOARD_UART_CLOCK_HZ     48000000u

void board_led_init(void);

// Full chip reset through the power manager watchdog. The GPU firmware then boots from the SD card again,
// which with the USB loader card brings the loader back. Never returns.
void board_reboot(void) __attribute__((noreturn));
void board_led_write(int on);

// Free-running 1 MHz system timer. Wraps every 71.6 minutes, so compare with unsigned subtraction.
uint32_t board_micros(void);
void board_delay_us(uint32_t us);
