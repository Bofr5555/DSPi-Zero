#pragma once

#include "pico.h"

// Single core. DSPi keeps its Core 1 worker idle unless an output past the first pair is enabled
// (main_z4.c pins core1_mode to idle), so these are never expected to run. Launching does nothing.
void multicore_launch_core1(void (*entry)(void));
void multicore_reset_core1(void);
void multicore_lockout_victim_init(void);
bool multicore_lockout_victim_is_initialized(uint core_num);
void multicore_lockout_start_blocking(void);
bool multicore_lockout_start_timeout_us(uint64_t timeout_us);
void multicore_lockout_end_blocking(void);
bool multicore_lockout_end_timeout_us(uint64_t timeout_us);
void multicore_fifo_push_blocking(uint32_t data);
uint32_t multicore_fifo_pop_blocking(void);
bool multicore_fifo_rvalid(void);
bool multicore_fifo_wready(void);
void multicore_fifo_drain(void);
