#include "uart.h"

#include <stdbool.h>

#include "board.h"
#include "broadcom/defines.h"
#include "broadcom/gpio.h"

#define FR_BUSY   (1u << 3)
#define FR_TXFF   (1u << 5)
#define LCRH_FEN  (1u << 4)
#define LCRH_WLEN8 (3u << 5)
#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 8)
#define CR_RXE    (1u << 9)

// Every wait is time-bounded, so a dead UART loses log output instead of stopping the firmware.
#define BUSY_TIMEOUT_US  10000u
#define TXFF_TIMEOUT_US  2000u

static int uart_is_ready;

#define TX_RING_SIZE 2048u      // power of two
static char tx_ring[TX_RING_SIZE];
static uint32_t tx_head, tx_tail, tx_dropped;
static int tx_buffered;

void uart_init(uint32_t uart_clock_hz, uint32_t baud) {
    UART0->CR = 0;
    uint32_t start = board_micros();
    while ((UART0->FR & FR_BUSY) && board_micros() - start < BUSY_TIMEOUT_US) {}
    UART0->LCR_H = 0;           // also flushes the TX FIFO
    UART0->ICR = 0x7FF;

    // PL011 divisor is clk / (16 * baud) in 16.6 fixed point, rounded to nearest.
    uint32_t div = (uart_clock_hz * 4u + baud / 2u) / baud;
    UART0->IBRD = div >> 6;
    UART0->FBRD = div & 0x3F;
    UART0->LCR_H = LCRH_WLEN8 | LCRH_FEN;   // LCR_H write latches IBRD/FBRD

    // ALT0 on GPIO 14/15 is TXD0/RXD0. The firmware may have routed the mini UART there instead.
    gpio_set_function(BOARD_UART_TX_PIN, GPIO_FUNCTION_ALT0);
    gpio_set_function(BOARD_UART_RX_PIN, GPIO_FUNCTION_ALT0);

    UART0->CR = CR_UARTEN | CR_TXE | CR_RXE;
    uart_is_ready = 1;
}

int uart_ready(void) {
    return uart_is_ready;
}

void uart_set_buffered(int on) {
    if (!on) {
        // Drain what is queued, so blocking output that follows stays in order.
        while (tx_head != tx_tail) {
            uart_poll();
        }
    }
    tx_buffered = on;
}

void uart_poll(void) {
    while (tx_head != tx_tail && !(UART0->FR & FR_TXFF)) {
        UART0->DR = (uint32_t)tx_ring[tx_tail & (TX_RING_SIZE - 1u)];
        tx_tail++;
    }
}

uint32_t uart_dropped(void) {
    return tx_dropped;
}

void uart_putc(char c) {
    if (c == '\n') {
        uart_putc('\r');
    }
    if (tx_buffered) {
        if (tx_head - tx_tail < TX_RING_SIZE) {
            tx_ring[tx_head & (TX_RING_SIZE - 1u)] = c;
            tx_head++;
        } else {
            tx_dropped++;
        }
        return;
    }
    uint32_t start = board_micros();
    while (UART0->FR & FR_TXFF) {
        if (board_micros() - start > TXFF_TIMEOUT_US) {
            return;
        }
    }
    UART0->DR = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_put_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (i) {
        uart_putc(buf[--i]);
    }
}

void uart_put_hex(uint32_t v) {
    // Computed digits, no lookup table, so the load check can print before .rodata is trusted.
    uart_putc('0');
    uart_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t n = (v >> shift) & 0xF;
        uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}
