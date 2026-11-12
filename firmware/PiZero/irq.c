// GPU interrupt dispatch for images that run with interrupts on. startup.S saves state and calls
// irq_handler(). TinyUSB's BP_EnableIRQ/BP_DisableIRQ hooks map onto the same controller.

#include <stdbool.h>

#include "irq.h"

#include "broadcom/defines.h"
#include "broadcom/interrupts.h"

static irq_fn_t handlers[64];
static volatile uint32_t taken, spurious;

void irq_set_handler(uint32_t irq, irq_fn_t fn) {
    if (irq < 64u) {
        handlers[irq] = fn;
    }
}

void irq_enable(uint32_t irq) {
    if (irq < 32u) {
        LIC->ENABLE_1 = 1u << irq;
    } else if (irq < 64u) {
        LIC->ENABLE_2 = 1u << (irq - 32u);
    }
}

void irq_disable(uint32_t irq) {
    if (irq < 32u) {
        LIC->DISABLE_1 = 1u << irq;
    } else if (irq < 64u) {
        LIC->DISABLE_2 = 1u << (irq - 32u);
    }
}

void irq_global_enable(void) {
    __asm__ volatile("cpsie i" ::: "memory");
}

uint32_t irq_global_disable(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr)::"memory");
    return cpsr;
}

void irq_global_restore(uint32_t cpsr) {
    if (!(cpsr & 0x80u)) {
        __asm__ volatile("cpsie i" ::: "memory");
    }
}

uint32_t irq_count(void) {
    return taken;
}

uint32_t irq_spurious(void) {
    return spurious;
}

static volatile uint32_t cycles_in;

uint32_t irq_cycles(void) {
    return cycles_in;
}

static inline uint32_t cycle_count(void) {
#if __ARM_ARCH >= 7
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));   // PMCCNTR, if the image enabled it
    return v;
#else
    return 0;
#endif
}

void irq_handler(void) {
    uint32_t c0 = cycle_count();
    taken++;
    bool handled = false;
    uint32_t p1 = LIC->PENDING_1 & LIC->ENABLE_1;
    uint32_t p2 = LIC->PENDING_2 & LIC->ENABLE_2;
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t bit = i < 32u ? (p1 >> i) & 1u : (p2 >> (i - 32u)) & 1u;
        if (bit && handlers[i]) {
            handlers[i]();
            handled = true;
        }
    }
    if (!handled) {
        spurious++;
    }
    cycles_in += cycle_count() - c0;
}

// TinyUSB's DWC2 glue masks the USB interrupt around its event queue with these.
void BP_EnableIRQ(IRQn_Type irq) {
    irq_enable((uint32_t)irq);
}

void BP_DisableIRQ(IRQn_Type irq) {
    irq_disable((uint32_t)irq);
}
