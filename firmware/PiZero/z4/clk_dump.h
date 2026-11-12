#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CLK_DUMP_MAGIC 0x444B4C43u   // "CLKD"
#define CLK_DUMP_MAX_REGS 160u
#define CLK_DUMP_MBOX_IDS 14u        // firmware clock ids 1 (EMMC) to 14 (PIXEL_BVB)

// Bench request 0x4C reply: firmware clock rates, then (offset, value at boot, value now) per register.
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t mbox_set[CLK_DUMP_MBOX_IDS];        // GET_CLOCK_RATE, 0 if not asked or not answered
    uint32_t mbox_measured[CLK_DUMP_MBOX_IDS];   // GET_CLOCK_RATE_MEASURED
    struct {
        uint32_t offset;
        uint32_t boot;
        uint32_t now;
    } entry[CLK_DUMP_MAX_REGS];
} clk_dump_t;

void clk_dump_boot_snapshot(void);      // first thing in main(), before any clock is changed
uint32_t clk_dump_fill(clk_dump_t *out, bool with_mailbox);   // returns the bytes to send
