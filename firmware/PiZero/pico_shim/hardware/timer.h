#pragma once

#include "hardware/address_mapped.h"

// time_us_32/64 read the BCM 1 MHz free-running system timer, the same clock as board_micros().
uint32_t time_us_32(void);
uint64_t time_us_64(void);
void busy_wait_us_32(uint32_t delay_us);
void busy_wait_us(uint64_t delay_us);
void busy_wait_ms(uint32_t delay_ms);

// Layout of the RP2350 timer block's fields DSPi touches. PICO_DEFAULT_TIMER_INSTANCE() points at a
// RAM stand-in that drives nothing; the alarm functions below fail to claim, so no alarm is ever used.
typedef struct {
    io_wo_32 timehw;
    io_wo_32 timelw;
    io_ro_32 timehr;
    io_ro_32 timelr;
    io_rw_32 alarm[4];
    io_rw_32 armed;
    io_ro_32 timerawh;
    io_ro_32 timerawl;
    io_rw_32 dbgpause;
    io_rw_32 pause;
    io_rw_32 locked;
    io_rw_32 source;
    io_rw_32 intr;
    io_rw_32 inte;
    io_rw_32 intf;
    io_ro_32 ints;
} timer_hw_t;

extern timer_hw_t pico_shim_timer_hw;
#define timer_hw (&pico_shim_timer_hw)
#define PICO_DEFAULT_TIMER_INSTANCE() timer_hw

int hardware_alarm_claim_unused(bool required);
uint hardware_alarm_get_irq_num(uint alarm_num);
