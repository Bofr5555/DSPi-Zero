#pragma once

// BCM2837 only: identity-maps SDRAM as cached Normal memory and the peripherals as Device memory, then
// turns on the MMU and D-cache. Call once, early, with interrupts off. With it on, buffers the GPU
// reads (the mailbox) need data_clean / data_invalidate from broadcom/caches.h.
void mmu_enable(void);
