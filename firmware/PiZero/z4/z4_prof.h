#pragma once

#include <stdbool.h>
#include <stdint.h>

#define Z4_PROF_MAX 16384u

extern volatile bool z4_prof_in_block;
extern uint32_t z4_prof_samples[Z4_PROF_MAX];
extern volatile uint32_t z4_prof_count;

// Starts sampling every period microseconds, clearing earlier samples. A period of 0 stops it.
void z4_prof_start(uint32_t period);
