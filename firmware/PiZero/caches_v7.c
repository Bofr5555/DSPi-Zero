// BCM2837 (AArch32) cache helpers, in place of broadcom-peripherals' AArch64-only ones, and the MMU setup
// that turns the D-cache on (mmu_enable). The helpers do nothing while the D-cache is off.

#include <stdbool.h>
#include <stdint.h>

#include "broadcom/caches.h"
#include "mmu.h"

#define LINE 64u    // Cortex-A53 L1 D-cache and L2 line size

static bool dcache_on(void) {
    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    return (sctlr & (1u << 2)) != 0;
}

void init_caches(void) {}

void data_clean(volatile void *starting_address, size_t size) {
    if (!dcache_on()) {
        return;
    }
    uintptr_t a = (uintptr_t)starting_address & ~(uintptr_t)(LINE - 1);
    for (uintptr_t end = (uintptr_t)starting_address + size; a < end; a += LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(a));    // DCCMVAC
    }
    __asm__ volatile("dsb" ::: "memory");
}

void data_clean_and_invalidate(volatile void *starting_address, size_t size) {
    if (!dcache_on()) {
        return;
    }
    uintptr_t a = (uintptr_t)starting_address & ~(uintptr_t)(LINE - 1);
    for (uintptr_t end = (uintptr_t)starting_address + size; a < end; a += LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(a));    // DCCIMVAC
    }
    __asm__ volatile("dsb" ::: "memory");
}

void data_invalidate(volatile void *starting_address, size_t size) {
    // Clean as well, so a partly covered line never loses neighbouring dirty data.
    data_clean_and_invalidate(starting_address, size);
}

// Invalidate every data/unified cache level by set/way, as CLIDR and CCSIDR describe them.
static void dcache_invalidate_all(void) {
    uint32_t clidr;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));
    for (uint32_t level = 0; level < 7; level++) {
        uint32_t ctype = (clidr >> (level * 3)) & 7u;
        if (ctype < 2) {
            continue;   // no cache, or instruction only
        }
        uint32_t ccsidr;
        __asm__ volatile("mcr p15, 2, %0, c0, c0, 0\n\tisb" ::"r"(level << 1) : "memory");   // CSSELR
        __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));                           // CCSIDR
        uint32_t line_shift = (ccsidr & 7u) + 4u;
        uint32_t ways = ((ccsidr >> 3) & 0x3FFu) + 1u;
        uint32_t sets = ((ccsidr >> 13) & 0x7FFFu) + 1u;
        uint32_t way_shift = ways > 1u ? (uint32_t)__builtin_clz(ways - 1u) : 0u;
        for (uint32_t w = 0; w < ways; w++) {
            for (uint32_t s = 0; s < sets; s++) {
                uint32_t sw = (w << way_shift) | (s << line_shift) | (level << 1);
                __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" ::"r"(sw));   // DCISW
            }
        }
    }
    __asm__ volatile("dsb" ::: "memory");
}

// Short-descriptor 1 MB sections: SDRAM below the peripherals as Normal write-back write-allocate,
// peripherals and the ARM local block as Device and execute-never, everything else unmapped.
static uint32_t ttb[4096] __attribute__((aligned(16384)));

#define SECT        0x2u
#define SECT_B      (1u << 2)
#define SECT_C      (1u << 3)
#define SECT_XN     (1u << 4)
#define SECT_AP_RW  (3u << 10)
#define SECT_TEX1   (1u << 12)
#define PERIPH_BASE 0x3F000000u
#define LOCAL_END   0x40100000u

extern uint8_t __nocache_start[], __nocache_end[];  // pizero_sections.ld, 1 MB aligned

void mmu_enable(void) {
    for (uint32_t i = 0; i < 4096u; i++) {
        uint32_t base = i << 20;
        if (base >= (uint32_t)__nocache_start && base < (uint32_t)__nocache_end) {
            ttb[i] = base | SECT | SECT_AP_RW | SECT_TEX1 | SECT_XN;    // Normal, non-cacheable
        } else if (base < PERIPH_BASE) {
            ttb[i] = base | SECT | SECT_AP_RW | SECT_TEX1 | SECT_C | SECT_B;
        } else if (base < LOCAL_END) {
            ttb[i] = base | SECT | SECT_AP_RW | SECT_B | SECT_XN;
        } else {
            ttb[i] = 0;
        }
    }
    dcache_invalidate_all();
    uint32_t zero = 0;
    __asm__ volatile(
        "mcr p15, 0, %0, c8, c7, 0\n\t"     // TLBIALL
        "mcr p15, 0, %0, c7, c5, 0\n\t"     // ICIALLU
        "mcr p15, 0, %0, c7, c5, 6\n\t"     // BPIALL
        "mcr p15, 0, %0, c2, c0, 2\n\t"     // TTBCR = 0: TTBR0 covers everything
        "mcr p15, 0, %1, c2, c0, 0\n\t"     // TTBR0 (table walks uncached)
        "mcr p15, 0, %2, c3, c0, 0\n\t"     // DACR: every domain client
        "dsb\n\tisb"
        :
        : "r"(zero), "r"((uint32_t)ttb), "r"(0x55555555u)
        : "memory");
    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr |= (1u << 0) | (1u << 2) | (1u << 11) | (1u << 12);   // M, C, Z, I
    sctlr &= ~((1u << 1) | (1u << 28) | (1u << 29));            // no alignment fault, TEX remap or AFE
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n\tisb" ::"r"(sctlr) : "memory");
}
