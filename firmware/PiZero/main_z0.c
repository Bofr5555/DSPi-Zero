// Milestone Z0: ACT LED blinks at 1 Hz and UART0 prints a heartbeat with mailbox clock rates.
// Bring-up steps and the LED failure codes are in Documentation/Porting/PiZero_Z0_bringup.md.

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "bootcheck.h"
#include "mbox.h"
#include "uart.h"

#include "broadcom/gen/vcmailbox.h"

static void print_hz(const char *label, uint32_t hz) {
    uart_puts(label);
    uart_put_dec(hz);
    uart_puts(" Hz");
}

int main(void) {
    board_led_init();

    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    boot_check(true);
    uart_puts("\n\nDSPi PiZero Z0 bring-up (" __DATE__ " " __TIME__ ")\n");

    uart_puts("board revision ");
    uart_put_hex(mbox_board_revision());
    uart_puts(", firmware revision ");
    uart_put_hex(mbox_firmware_revision());
    uart_puts("\n");

    uint32_t mem_base = 0, mem_size = 0;
    if (mbox_arm_memory(&mem_base, &mem_size)) {
        uart_puts("ARM memory base ");
        uart_put_hex(mem_base);
        uart_puts(" size ");
        uart_put_dec(mem_size >> 20);
        uart_puts(" MB\n");
    } else {
        uart_puts("ARM memory query failed\n");
    }

    print_hz("set rates: arm ", mbox_clock_rate(VCMAILBOX_CLOCK_ARM));
    print_hz(", core ", mbox_clock_rate(VCMAILBOX_CLOCK_CORE));
    print_hz(", uart ", mbox_clock_rate(VCMAILBOX_CLOCK_UART));
    uart_puts("\n");

    uint32_t last_toggle = board_micros();
    uint32_t last_beat = last_toggle;
    uint32_t seconds = 0;
    int led_on = 1;

    for (;;) {
        uint32_t now = board_micros();
        if (now - last_toggle >= 500000u) {
            last_toggle += 500000u;
            led_on = !led_on;
            board_led_write(led_on);
        }
        if (now - last_beat >= 1000000u) {
            last_beat += 1000000u;
            seconds++;
            uart_puts("hb ");
            uart_put_dec(seconds);
            print_hz(" s: arm ", mbox_clock_rate_measured(VCMAILBOX_CLOCK_ARM));
            print_hz(", core ", mbox_clock_rate_measured(VCMAILBOX_CLOCK_CORE));
            uart_puts(" (measured)\n");
        }
    }
}
