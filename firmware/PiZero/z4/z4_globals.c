// Definitions for the globals DSPi's main.c provides (see z4_globals.h). Types and
// initial values follow those files.

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "usb_audio.h"

#include "z4_globals.h"

// main.c
usb_feedback_ctrl_t fb_ctrl;
volatile uint32_t feedback_10_14 = 0;
volatile uint32_t nominal_feedback_10_14 = 0;
volatile bool output_type_switch_in_progress = false;
volatile int overruns = 0;
volatile uint32_t pio_samples_dma = 0;
volatile uint32_t pdm_ring_overruns = 0;
volatile uint32_t pdm_ring_underruns = 0;
volatile uint32_t pdm_dma_overruns = 0;
volatile uint32_t pdm_dma_underruns = 0;
volatile uint32_t spdif_underruns = 0;
volatile uint32_t usb_audio_packets = 0;
volatile uint32_t usb_audio_alt_set = 0;
volatile uint32_t usb_audio_mounted = 0;
