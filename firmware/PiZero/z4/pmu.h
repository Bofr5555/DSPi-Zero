#pragma once

// Cortex-A53 performance counters (AArch32, PL1): the cycle counter plus five event counters, used to
// split DSPi's block time into instructions, cache refills and interrupt time. BCM2837 only.

#include <stdint.h>

enum {
    PMU_EV_INST = 0x08,         // instructions architecturally executed
    PMU_EV_L1I_REFILL = 0x01,
    PMU_EV_L1D_REFILL = 0x03,
    PMU_EV_L2D_REFILL = 0x17,
    PMU_EV_BR_MISPRED = 0x10,
};

#define PMU_N_EVENTS 5

typedef struct {
    uint32_t cycles;
    uint32_t ev[PMU_N_EVENTS];
} pmu_snap_t;

static const uint8_t pmu_events[PMU_N_EVENTS] = {
    PMU_EV_INST, PMU_EV_L1I_REFILL, PMU_EV_L1D_REFILL, PMU_EV_L2D_REFILL, PMU_EV_BR_MISPRED,
};

// Returns the number of event counters the core has; 0 means no PMU use.
static inline uint32_t pmu_init(void) {
    uint32_t pmcr;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    uint32_t n = (pmcr >> 11) & 31u;
    if (n < PMU_N_EVENTS) {
        return 0;
    }
    for (uint32_t i = 0; i < PMU_N_EVENTS; i++) {
        __asm__ volatile("mcr p15, 0, %0, c9, c12, 5\n\tisb" ::"r"(i));                 // PMSELR
        __asm__ volatile("mcr p15, 0, %0, c9, c13, 1" ::"r"((uint32_t)pmu_events[i]));  // PMXEVTYPER
    }
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" ::"r"(0x80000000u | ((1u << PMU_N_EVENTS) - 1u)));  // PMCNTENSET
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0\n\tisb" ::"r"((pmcr & ~0x3Fu) | 0x7u));  // E, reset P and C
    return n;
}

static inline uint32_t pmu_cycles(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

static inline void pmu_read(pmu_snap_t *s) {
    for (uint32_t i = 0; i < PMU_N_EVENTS; i++) {
        uint32_t v;
        __asm__ volatile("mcr p15, 0, %1, c9, c12, 5\n\tisb\n\tmrc p15, 0, %0, c9, c13, 2" : "=r"(v) : "r"(i));
        s->ev[i] = v;
    }
    s->cycles = pmu_cycles();
}
