// Bench USB-to-UART bridge for Pi Zero bring-up on a Raspberry Pi Pico (RP2040).
// UART0 on GP0 (TX) / GP1 (RX) at 115200 8N1 <-> USB CDC. Wiring is in PiZero_Z0_bringup.md section 5.

#include "pico/stdlib.h"
#include "hardware/uart.h"

#define BRIDGE_UART   uart0
#define BRIDGE_TX_PIN 0
#define BRIDGE_RX_PIN 1
#define BRIDGE_BAUD   115200

int main(void) {
    stdio_usb_init();

    uart_init(BRIDGE_UART, BRIDGE_BAUD);
    gpio_set_function(BRIDGE_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BRIDGE_RX_PIN, GPIO_FUNC_UART);
    // The pull-up holds RX idle-high while the Zero is unpowered, so no junk bytes appear.
    gpio_pull_up(BRIDGE_RX_PIN);

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    for (;;) {
        while (uart_is_readable(BRIDGE_UART)) {
            putchar_raw(uart_getc(BRIDGE_UART));
        }
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            uart_putc_raw(BRIDGE_UART, (char)c);
        }
    }
}
