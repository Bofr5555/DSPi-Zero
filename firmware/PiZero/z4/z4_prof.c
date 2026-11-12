// Sampling profiler for the bench: system timer channel 3 interrupts about every 37 us and records the
// interrupted PC while process_input_block() runs. tools/z4_prof.py reads the samples and maps them to
// functions with the ELF's symbols.

#include <stdbool.h>
#include <stdint.h>

#include "irq.h"
#include "z4_prof.h"

#include "broadcom/defines.h"
#include "broadcom/interrupts.h"

extern volatile uint32_t irq_return_pc;     // startup.S

volatile bool z4_prof_in_block;
uint32_t z4_prof_samples[Z4_PROF_MAX];
volatile uint32_t z4_prof_count;
static uint32_t period_us;
static uint32_t jitter;

static void prof_irq(void) {
    SYSTMR->CS = 1u << 3;
    if (z4_prof_in_block && z4_prof_count < Z4_PROF_MAX) {
        z4_prof_samples[z4_prof_count++] = irq_return_pc;
    }
    // A small varying step, so the samples do not lock to the 1 ms block rhythm.
    jitter = jitter * 1103515245u + 12345u;
    SYSTMR->C3 = SYSTMR->CLO + period_us + ((jitter >> 16) & 7u);
    (void)SYSTMR->CS;   // let the clear land before returning, or the IRQ fires again with nothing pending
}

void z4_prof_start(uint32_t period) {
    if (period == 0) {
        irq_disable(TIMER_3_IRQn);
        period_us = 0;
        return;
    }
    period_us = period < 10u ? 10u : period;
    z4_prof_count = 0;
    irq_set_handler(TIMER_3_IRQn, prof_irq);
    SYSTMR->CS = 1u << 3;
    SYSTMR->C3 = SYSTMR->CLO + period_us;
    irq_enable(TIMER_3_IRQn);
}
