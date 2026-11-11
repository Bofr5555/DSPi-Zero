#include "bootcheck.h"

#include <stdint.h>

#include "uart.h"

// Every character here is computed or an immediate, so the report is readable even when .rodata
// is not where the linker put it. See PiZero_Z0_bringup.md section 6.

extern uint32_t boot_info[4];   // firmware r0, r1, r2 and the address _start actually ran at
extern const uint8_t __loadcheck_start[], __loadcheck_end[];

// Prints up to four characters packed into an immediate.
static void say(uint32_t packed) {
    while (packed) {
        uart_putc((char)(packed & 0xFF));
        packed >>= 8;
    }
}
#define S4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static void dump16(uint32_t addr) {
    uart_put_hex(addr);
    uart_putc(':');
    for (int i = 0; i < 16; i++) {
        uint32_t b = ((volatile const uint8_t *)addr)[i];
        uart_putc(' ');
        uart_putc((char)((b >> 4) < 10 ? '0' + (b >> 4) : 'a' + (b >> 4) - 10));
        uart_putc((char)((b & 15) < 10 ? '0' + (b & 15) : 'a' + (b & 15) - 10));
    }
    uart_putc('\n');
}

void boot_check(bool verbose) {
    say(S4('\n', 'L', 'C', ' '));
    say(S4('r', '0', '=', 0));
    uart_put_hex(boot_info[0]);
    say(S4(' ', 'r', '1', '='));
    uart_put_hex(boot_info[1]);
    say(S4(' ', 'r', '2', '='));
    uart_put_hex(boot_info[2]);
    say(S4(' ', 'r', 'u', 'n'));
    say(S4('@', 0, 0, 0));
    uart_put_hex(boot_info[3]);
    uart_putc('\n');

    uint32_t lc_start = (uint32_t)__loadcheck_start;
    uint32_t lc_end = (uint32_t)__loadcheck_end;
    uint32_t tail = lc_end - 4;
    uint32_t bad = 0, first_bad = 0;
    for (uint32_t a = lc_start; a < tail; a++) {
        if (*(volatile const uint8_t *)a != 0xA5) {
            if (!bad) {
                first_bad = a;
            }
            bad++;
        }
    }
    say(S4('L', 'C', ' ', 'p'));
    say(S4('a', 'd', ' ', 0));
    uart_put_hex(lc_start);
    uart_putc('-');
    uart_put_hex(lc_end);
    say(S4(' ', 'b', 'a', 'd'));
    uart_putc('=');
    uart_put_dec(bad);
    say(S4(' ', '1', 's', 't'));
    uart_putc('=');
    uart_put_hex(first_bad);
    say(S4(' ', 't', 'a', 'i'));
    say(S4('l', '=', 0, 0));
    uart_put_hex(*(volatile const uint32_t *)tail);
    uart_putc('\n');

    if (!verbose && bad == 0 && boot_info[3] == 0x8000) {
        return;
    }
    // Start of code, then every 64 bytes from late .text through .rodata into the filler.
    dump16(0x8000);
    uint32_t from = (lc_start > 0x8d00 + 0x400) ? lc_start - 0x400 : 0x8d00;
    for (uint32_t a = from & ~0x3Fu; a < lc_start + 64; a += 0x40) {
        dump16(a);
    }
    dump16(tail - 12);
}
