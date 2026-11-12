// Clock register dump for the PLLA work (PiZero_PLLA_plan.md phase 0). Read-only: every clock-manager and
// A2W register that Linux's clk-bcm2835.c names, captured once at boot before anything changes clocks,
// and again on request. Offsets are from the clock manager base, 0x3F101000; A2W starts at +0x1000.

#include <stdint.h>

#include "clk_dump.h"
#include "mbox.h"

#define CPRMAN 0x3F101000u

static const uint16_t regs[] = {
    // Clock-manager CTL and DIV pairs.
    0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020, 0x024, 0x028, 0x02c, 0x030, 0x034,
    0x038, 0x03c, 0x040, 0x044, 0x048, 0x04c, 0x050, 0x054, 0x058, 0x05c, 0x060, 0x064, 0x068, 0x06c,
    0x070, 0x074, 0x078, 0x07c, 0x080, 0x084, 0x088, 0x08c, 0x090, 0x094, 0x098, 0x09c, 0x0a0, 0x0a4,
    0x0a8, 0x0ac, 0x0b0, 0x0b4, 0x0c8, 0x0cc, 0x0d0, 0x0d4, 0x0d8, 0x0dc, 0x0e0, 0x0e4, 0x0e8, 0x0ec,
    0x0f0, 0x0f4, 0x0f8, 0x0fc, 0x158, 0x15c, 0x160, 0x164, 0x168, 0x16c, 0x190, 0x194, 0x1a8, 0x1ac,
    0x1b0, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1d0, 0x1d4,
    // PLL control in the clock manager, and the lock bits.
    0x104, 0x108, 0x10c, 0x110, 0x170, 0x114,
    // A2W per PLL (A, C, D, H, B): CTRL, FRAC, then ANA0-3 twice so a live readback shows.
    0x1100, 0x1200, 0x1010, 0x1014, 0x1018, 0x101c, 0x1010, 0x1014, 0x1018, 0x101c,
    0x1120, 0x1220, 0x1030, 0x1034, 0x1038, 0x103c, 0x1030, 0x1034, 0x1038, 0x103c,
    0x1140, 0x1240, 0x1050, 0x1054, 0x1058, 0x105c, 0x1050, 0x1054, 0x1058, 0x105c,
    0x1160, 0x1260, 0x1070, 0x1074, 0x1078, 0x107c, 0x1070, 0x1074, 0x1078, 0x107c,
    0x11e0, 0x12e0, 0x10f0, 0x10f4, 0x10f8, 0x10fc, 0x10f0, 0x10f4, 0x10f8, 0x10fc,
    // A2W channel dividers.
    0x1300, 0x1400, 0x1500, 0x1600,             // PLLA DSI0, CORE, PER, CCP2
    0x1320, 0x1420, 0x1520, 0x1620,             // PLLC CORE2, CORE1, PER, CORE0
    0x1340, 0x1440, 0x1540, 0x1640,             // PLLD DSI0, CORE, PER, DSI1
    0x1360, 0x1460, 0x1560, 0x1660,             // PLLH AUX, RCAL, PIX, STS
    0x13e0,                                     // PLLB ARM
    0x1190,                                     // A2W XOSC_CTRL
};

#define N_REGS (sizeof(regs) / sizeof(regs[0]))
_Static_assert(N_REGS <= CLK_DUMP_MAX_REGS, "clk_dump table");

static uint32_t boot_values[N_REGS];

static uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(CPRMAN + off);
}

void clk_dump_boot_snapshot(void) {
    for (uint32_t i = 0; i < N_REGS; i++) {
        boot_values[i] = rd(regs[i]);
    }
}

uint32_t clk_dump_fill(clk_dump_t *out, bool with_mailbox) {
    out->magic = CLK_DUMP_MAGIC;
    out->count = N_REGS;
    for (uint32_t i = 0; i < N_REGS; i++) {
        out->entry[i].offset = regs[i];
        out->entry[i].boot = boot_values[i];
        out->entry[i].now = rd(regs[i]);
    }
    for (uint32_t id = 0; id < CLK_DUMP_MBOX_IDS; id++) {
        out->mbox_set[id] = with_mailbox ? mbox_clock_rate(id + 1u) : 0;
        out->mbox_measured[id] = with_mailbox ? mbox_clock_rate_measured(id + 1u) : 0;
    }
    return (uint32_t)(sizeof(*out) - sizeof(out->entry) + N_REGS * sizeof(out->entry[0]));
}
