#pragma once

#include "pico.h"

// Barriers and events are the real instructions. ARMv6 (BCM2835) has no DMB/DSB/ISB, so it uses the
// equivalent CP15 operations.
#if __ARM_ARCH >= 7
static __force_inline void __dmb(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}
static __force_inline void __dsb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}
static __force_inline void __isb(void) {
    __asm__ volatile("isb" ::: "memory");
}
#else
static __force_inline void __dmb(void) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" ::"r"(0) : "memory");
}
static __force_inline void __dsb(void) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}
static __force_inline void __isb(void) {
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" ::"r"(0) : "memory");
}
#endif

static __force_inline void __sev(void) {
    __asm__ volatile("sev");
}
static __force_inline void __wfe(void) {
    __asm__ volatile("wfe" ::: "memory");
}
static __force_inline void __wfi(void) {
    __asm__ volatile("wfi" ::: "memory");
}
static __force_inline void __nop(void) {
    __asm__ volatile("nop");
}

// The SDK masks PRIMASK. Here the CPSR I bit plays that role: the saved value is the whole CPSR and
// restore only clears I again if it was clear when saved.
static __force_inline uint32_t save_and_disable_interrupts(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr)::"memory");
    return cpsr;
}

static __force_inline void restore_interrupts(uint32_t status) {
    if (!(status & 0x80u)) {
        __asm__ volatile("cpsie i" ::: "memory");
    }
}

static __force_inline void restore_interrupts_from_disabled(uint32_t status) {
    restore_interrupts(status);
}

// Spin locks have no second core to exclude, so they only mask interrupts.
typedef volatile uint32_t spin_lock_t;
spin_lock_t *spin_lock_instance(uint lock_num);
uint spin_lock_claim_unused(bool required);
static __force_inline uint32_t spin_lock_blocking(spin_lock_t *lock) {
    (void)lock;
    return save_and_disable_interrupts();
}
static __force_inline void spin_unlock(spin_lock_t *lock, uint32_t saved_irq) {
    (void)lock;
    restore_interrupts(saved_irq);
}
