#pragma once

#include "pico/types.h"

// The whole image runs from SDRAM, so the SDK's RAM placement attributes only name sections.
// .time_critical* is collected into .text by pizero_sections.ld.
#define __not_in_flash(group)               __attribute__((section(".time_critical." group)))
#define __not_in_flash_func(func_name)      __not_in_flash(__STRING(func_name)) func_name
#define __no_inline_not_in_flash_func(func_name) __attribute__((noinline)) __not_in_flash_func(func_name)
#define __time_critical_func(func_name)     __not_in_flash_func(func_name)
#define __in_flash(group)                   __attribute__((section(".text.flash." group)))
#define __scratch_x(group)                  __attribute__((section(".scratch_x." group)))
#define __scratch_y(group)                  __attribute__((section(".scratch_y." group)))
#define __uninitialized_ram(group)          __attribute__((section(".bss.uninit." #group))) group

#ifndef __STRING
#define __STRING(x) #x
#endif
#ifndef __force_inline
#define __force_inline inline __attribute__((always_inline))
#endif
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif

#ifndef count_of
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((b) < (a) ? (b) : (a))
#endif
#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")

static inline void tight_loop_contents(void) {}

// Single core: everything runs as the SDK's core 0.
static inline uint get_core_num(void) {
    return 0;
}

// The SDK returns the active exception number, 0 in thread mode. Here: 0 in the system (or user)
// mode the main loop runs in, otherwise the CPSR mode bits, which are never 0.
static inline uint __get_current_exception(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    uint32_t mode = cpsr & 0x1fu;
    return (mode == 0x1fu || mode == 0x10u) ? 0u : mode;
}

// Prints the message on the UART and stops with the fault LED pattern.
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void panic_unsupported(void) __attribute__((noreturn));
