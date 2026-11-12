#pragma once

#include "hardware/address_mapped.h"

// RP2350 NVIC enable registers. A RAM stand-in: DSPi's selective flash blackout writes them, and
// nothing reads them back into the Pi's interrupt controller.
typedef struct {
    io_rw_32 iser[2];
    io_rw_32 icer[2];
    io_rw_32 ispr[2];
    io_rw_32 icpr[2];
} nvic_hw_t;

extern nvic_hw_t pico_shim_nvic_hw;
#define nvic_hw (&pico_shim_nvic_hw)
