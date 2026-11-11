#pragma once

#include <stdint.h>

// ARM interrupts through the legacy BCM interrupt controller. GPU IRQs reach core 0 by default on the
// BCM2837 too, and nothing here touches the per-core local controller.
#define IRQ_USB 9u      // GPU IRQ 9, the DWC2 USB controller

typedef void (*irq_fn_t)(void);

void irq_set_handler(uint32_t irq, irq_fn_t fn);   // GPU IRQs 0-63 only
void irq_enable(uint32_t irq);
void irq_disable(uint32_t irq);
void irq_global_enable(void);       // CPSR.I clear
uint32_t irq_global_disable(void);  // returns the previous CPSR for irq_global_restore
void irq_global_restore(uint32_t cpsr);
uint32_t irq_count(void);           // IRQs taken, for bench checks
uint32_t irq_spurious(void);        // IRQs with no registered handler pending
