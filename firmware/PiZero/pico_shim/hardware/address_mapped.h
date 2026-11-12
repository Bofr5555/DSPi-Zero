#pragma once

#include "pico.h"

typedef volatile uint32_t io_rw_32;
typedef const volatile uint32_t io_ro_32;
typedef volatile uint32_t io_wo_32;

// The register blocks behind these are plain RAM stand-ins (see pico_shim.c), so the SDK's atomic
// set and clear aliases become read-modify-write.
static inline void hw_set_bits(io_rw_32 *addr, uint32_t mask) {
    *addr |= mask;
}
static inline void hw_clear_bits(io_rw_32 *addr, uint32_t mask) {
    *addr &= ~mask;
}
static inline void hw_xor_bits(io_rw_32 *addr, uint32_t mask) {
    *addr ^= mask;
}
static inline void hw_write_masked(io_rw_32 *addr, uint32_t values, uint32_t write_mask) {
    *addr = (*addr & ~write_mask) | (values & write_mask);
}
