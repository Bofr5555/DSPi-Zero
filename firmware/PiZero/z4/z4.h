#pragma once

// Interface between the UAC2 front end (main_z4.c) and the port of DSPi's main loop (z4_mainloop.c).

#include <stdbool.h>
#include <stdint.h>

// main_z4.c: one pass of the audio side, which the settle loops call while they wait for a fade.
// USB task, stream consumer, test-signal pump, idle ring pump and notification drain.
void z4_service_producer(void);
bool z4_stream_running(void);   // the consumer is feeding received audio into the pipeline
uint32_t z4_host_rate(void);    // the rate the host set on the UAC2 clock

// z4_mainloop.c
void z4_dspi_init(void);        // main.c's core0_init(), less what needs RP2350 hardware
void z4_dspi_service(void);     // one pass of main.c's deferred work
void z4_stream_restart(void);   // a host stream is starting: host rate, then a synchronized restart
