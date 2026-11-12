#pragma once

#include "hardware/address_mapped.h"

// RP2350 UART API, for DSPi's UART control interface. Two RAM stand-ins: nothing is received and
// whatever is written goes nowhere. The SDK's uart_init is renamed: the Pi's own uart.h owns that name.
#define uart_init pico_uart_init

typedef struct {
    io_rw_32 dr;
    io_rw_32 rsr;
    uint32_t _pad0[4];
    io_ro_32 fr;
} uart_hw_t;

typedef struct uart_inst uart_inst_t;
extern uart_hw_t pico_shim_uart_hw[2];
#define uart0 ((uart_inst_t *)&pico_shim_uart_hw[0])
#define uart1 ((uart_inst_t *)&pico_shim_uart_hw[1])

typedef enum { UART_PARITY_NONE, UART_PARITY_EVEN, UART_PARITY_ODD } uart_parity_t;

#define UART0_IRQ 33u
#define UART1_IRQ 34u

static inline uart_hw_t *uart_get_hw(uart_inst_t *uart) {
    return (uart_hw_t *)uart;
}
static inline uint uart_get_index(uart_inst_t *uart) {
    return uart == uart1 ? 1u : 0u;
}

uint uart_init(uart_inst_t *uart, uint baudrate);
void uart_deinit(uart_inst_t *uart);
uint uart_set_baudrate(uart_inst_t *uart, uint baudrate);
void uart_set_format(uart_inst_t *uart, uint data_bits, uint stop_bits, uart_parity_t parity);
void uart_set_hw_flow(uart_inst_t *uart, bool cts, bool rts);
void uart_set_fifo_enabled(uart_inst_t *uart, bool enabled);
void uart_set_irq_enables(uart_inst_t *uart, bool rx_has_data, bool tx_needs_data);
bool uart_is_readable(uart_inst_t *uart);
bool uart_is_writable(uart_inst_t *uart);
