#pragma once

// Stereo I2S output on the BCM2837 PCM block (Z5): BCK on GPIO 18, LRCLK on GPIO 19, data on GPIO 21.
// The block is clock master at 64 fs, and a DMA channel loops over a ring of interleaved 32-bit L/R
// words (24-bit samples left-justified), as the RP2350's free-running I2S DMA does.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t running;
    uint32_t dma_channel;       // 0xFF before the first start
    uint32_t freq;              // requested sample rate
    uint32_t div_q12;           // clock manager divider, 12 fractional bits
    uint32_t tx_errors;         // PCM TX FIFO underflows seen
    uint32_t dma_errors;        // DMA DEBUG error flags seen
    uint32_t starts;
    uint32_t consumed;          // frames the DMA has read since the ring was bound
} pcm_out_status_t;

// Starts playing ring (ring_frames, a power of two) from frame start_frame. Interrupts must be masked.
bool pcm_out_start(const int32_t *ring, uint32_t ring_frames, uint32_t start_frame, uint32_t freq);
void pcm_out_stop(void);
void pcm_out_set_rate(uint32_t freq);
// Frames read so far, counted on from start_frame. Wraps are resolved against the system timer, so any
// gap between calls is fine.
uint32_t pcm_out_consumed_frames(void);
void pcm_out_status(pcm_out_status_t *out);
