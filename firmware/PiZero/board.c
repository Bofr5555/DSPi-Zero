#include "board.h"

#include <stdbool.h>

#include "uart.h"

#include "broadcom/defines.h"
#include "broadcom/gpio.h"

void board_led_write(int on) {
    gpio_set_value(BOARD_ACT_LED_PIN, BOARD_ACT_LED_ACTIVE_LOW ? !on : on);
}

uint32_t board_micros(void) {
    return SYSTMR->CLO;
}

void board_delay_us(uint32_t us) {
    uint32_t start = board_micros();
    while (board_micros() - start < us) {}
}

void board_led_init(void) {
    // Three quick flashes prove our code runs and owns the LED, before anything that could stall.
    gpio_set_function(BOARD_ACT_LED_PIN, GPIO_FUNCTION_OUTPUT);
    for (int i = 0; i < 3; i++) {
        board_led_write(1);
        board_delay_us(100000);
        board_led_write(0);
        board_delay_us(100000);
    }
    board_led_write(1);
}

// Power manager watchdog, at the peripheral base + 0x100000 on both chips. Every write needs the password.
#define BOARD_PM_BASE            (SYSTMR_BASE - 0x3000u + 0x100000u)
#define PM_RSTC            (*(volatile uint32_t *)(BOARD_PM_BASE + 0x1Cu))
#define PM_WDOG            (*(volatile uint32_t *)(BOARD_PM_BASE + 0x24u))
#define PM_PASSWORD        0x5A000000u
#define PM_RSTC_FULL_RESET 0x00000020u

void board_reboot(void) {
    if (uart_ready()) {
        uart_set_buffered(0);
    }
    PM_WDOG = PM_PASSWORD | 10u;     // about 150 us, in 16 us ticks
    PM_RSTC = PM_PASSWORD | (PM_RSTC & ~0x30u) | PM_RSTC_FULL_RESET;
    for (;;) {}
}

static const char *const fault_names[] = {
    "?", "undefined instruction", "SWI", "prefetch abort", "data abort", "reserved", "IRQ", "FIQ",
};

void fault_handler(uint32_t kind, uint32_t pc, uint32_t spsr, uint32_t dfsr);

// Images that enable interrupts link irq.c, which replaces this. Anything else treats an IRQ as a fault.
__attribute__((weak)) void irq_handler(void) {
    fault_handler(6, 0, 0, 0);
}

// Called from startup.S on any exception. No milestone enables interrupts yet, so IRQ and FIQ are faults too.
void fault_handler(uint32_t kind, uint32_t pc, uint32_t spsr, uint32_t dfsr) {
    if (uart_ready()) {
        uart_set_buffered(0);
        uart_puts("\nFAULT: ");
        uart_puts(fault_names[kind < 8 ? kind : 0]);
        uart_puts(" pc=");
        uart_put_hex(pc);
        uart_puts(" spsr=");
        uart_put_hex(spsr);
        uart_puts(" dfsr=");
        uart_put_hex(dfsr);
        uart_puts("\n");
    }
    for (;;) {
        board_led_write(1);
        board_delay_us(50000);
        board_led_write(0);
        board_delay_us(50000);
    }
}
