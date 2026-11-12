#pragma once

// All of DSPi's outputs on the SMI (secondary memory interface): a 16-bit parallel port that writes one
// word per cycle from a DMA loop, so every output pin changes on the same cycle. Each audio frame is 256
// words (256 fs). Register layout from Linux's bcm2835_smi.h; timing measured in bring-up section 19.

#include <stdbool.h>
#include <stdint.h>

#define SMI_FRAME_WORDS 256u

// Fills n frames starting at absolute frame `frame` into dst (n * SMI_FRAME_WORDS words). Called from the
// DMA interrupt, a fixed lead ahead of the frame the SMI is sending.
typedef void (*smi_compose_fn)(uint32_t frame, uint16_t *dst, uint32_t n);

typedef struct {
    uint32_t running;
    uint32_t dma_channel;
    uint32_t freq;
    uint32_t div_q8;        // SMI clock divider from 500 MHz PLLD, 8 fractional bits
    uint32_t frames;        // frames the DMA has read since the start
    uint32_t composed;      // frames composed (absolute)
    uint32_t late;          // times the composer fell behind the DMA
    uint32_t irqs;
    uint32_t cs;            // SMI CS
    uint32_t dma_debug;
    uint32_t compose_cycles;    // CPU cycles spent composing, wrapping (needs the PMU cycle counter on)
    uint32_t clean_cycles;      // and cleaning the composed frames out of the cache
} smi_out_status_t;

// Starts streaming ring (ring_frames frames, a power of two, 64-byte aligned, cleaned from the cache
// after each compose) on the GPIOs in pin_mask (SD0-SD15 are GPIO 8-23). Runs until stopped, composing as it goes.
bool smi_out_start(uint16_t *ring, uint32_t ring_frames, uint32_t freq, uint32_t pin_mask, smi_compose_fn compose);
void smi_out_set_rate(uint32_t freq);
uint32_t smi_out_frames(void);      // frames the DMA has read; safe from any context
uint32_t smi_out_composed(void);    // frames composed so far, the reader position of the slot rings
void smi_out_status(smi_out_status_t *out);

// Bench probe: samples GPIO gpio's level for ms milliseconds and returns the number of level changes.
uint32_t smi_out_probe_toggles(uint32_t gpio, uint32_t ms, uint32_t *samples);
