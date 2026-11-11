#pragma once

#include <stdint.h>

// Polled PL011 (UART0) on GPIO 14/15. Used for the bring-up log only.
void uart_init(uint32_t uart_clock_hz, uint32_t baud);
int uart_ready(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_dec(uint32_t v);
void uart_put_hex(uint32_t v);

// Buffered mode for loops that must not stall, such as USB polling. uart_putc then queues into a 2 KB
// ring (dropping on overflow) and uart_poll() moves what the TX FIFO can take. Off by default.
void uart_set_buffered(int on);
void uart_poll(void);
uint32_t uart_dropped(void);
