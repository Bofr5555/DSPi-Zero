// Stereo I2S output on the PCM block. Register layouts are from the BCM2835 ARM Peripherals datasheet
// (PCM/I2S chapter 8, DMA chapter 4, clock manager in section 6.3) and match the BCM2837. Settings follow
// Linux's bcm2835-i2s driver for Philips I2S, 2 x 32-bit slots per frame.

#include "pcm_out.h"

#include "mbox.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define CM_PCMCTL 0x3F101098u
#define CM_PCMDIV 0x3F10109Cu
#define CM_PASSWD (0x5Au << 24)
#define CM_SRC_PLLD 6u
#define CM_ENAB (1u << 4)
#define CM_KILL (1u << 5)
#define CM_BUSY (1u << 7)
#define CM_MASH1 (1u << 9)
#define PLLD_HZ 500000000u

#define PCM_CS 0x3F203000u
#define PCM_MODE 0x3F203008u
#define PCM_RXC 0x3F20300Cu
#define PCM_TXC 0x3F203010u
#define PCM_DREQ 0x3F203014u
#define PCM_INTEN 0x3F203018u
#define PCM_FIFO_BUS 0x7E203004u
#define CS_EN (1u << 0)
#define CS_TXON (1u << 2)
#define CS_TXCLR (1u << 3)
#define CS_DMAEN (1u << 9)
#define CS_TXERR (1u << 15)
#define CS_SYNC (1u << 24)
#define CS_STBY (1u << 25)

#define GPFSEL1 0x3F200004u
#define GPFSEL2 0x3F200008u

#define DMA_BASE 0x3F007000u
#define DMA_ENABLE 0x3F007FF0u
#define DMA_CS(ch) (DMA_BASE + 0x100u * (ch) + 0x00u)
#define DMA_CONBLK(ch) (DMA_BASE + 0x100u * (ch) + 0x04u)
#define DMA_SOURCE(ch) (DMA_BASE + 0x100u * (ch) + 0x0Cu)
#define DMA_DEBUG(ch) (DMA_BASE + 0x100u * (ch) + 0x20u)
#define DMA_CS_ACTIVE (1u << 0)
#define DMA_CS_END (1u << 1)
#define DMA_CS_RESET (1u << 31)
#define DMA_CS_WAIT_WRITES (1u << 28)
#define DMA_TI_WAIT_RESP (1u << 3)
#define DMA_TI_DEST_DREQ (1u << 6)
#define DMA_TI_SRC_INC (1u << 8)
#define DMA_TI_PERMAP_PCM_TX (2u << 16)
#define DMA_TI_NO_WIDE_BURSTS (1u << 26)

#define BUS(p) ((uint32_t)(p) | 0xC0000000u)   // SDRAM as the DMA sees it, uncached alias

#define CHUNKS 8u

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

// cbs[CHUNKS] is the entry block, from the start frame to the end of its chunk.
static dma_cb_t cbs[CHUNKS + 1] __attribute__((section(".nocache"), aligned(32)));

static pcm_out_status_t st = {.dma_channel = 0xFF};
static uint32_t ring_bus, ring_mask;
static uint32_t last_us;

static uint32_t micros(void) {
    return REG(0x3F003004u);    // system timer CLO
}

static void wait_us(uint32_t us) {
    uint32_t t0 = micros();
    while (micros() - t0 < us) {
    }
}

static void set_alt0(uint32_t reg, uint32_t shift) {
    REG(reg) = (REG(reg) & ~(7u << shift)) | (4u << shift);
}

// BCK = 64 fs from PLLD. MASH 1 dithers the 12-bit fraction, so the average rate is exact to 1/4096.
static void clock_set(uint32_t freq) {
    uint64_t bck = (uint64_t)freq * 64u;
    uint32_t div = (uint32_t)((((uint64_t)PLLD_HZ << 12) + bck / 2u) / bck);
    REG(CM_PCMCTL) = CM_PASSWD | CM_SRC_PLLD | CM_MASH1;
    uint32_t t0 = micros();
    while ((REG(CM_PCMCTL) & CM_BUSY) && micros() - t0 < 1000u) {
    }
    if (REG(CM_PCMCTL) & CM_BUSY) {
        REG(CM_PCMCTL) = CM_PASSWD | CM_KILL;
        wait_us(10);
    }
    REG(CM_PCMDIV) = CM_PASSWD | div;
    REG(CM_PCMCTL) = CM_PASSWD | CM_SRC_PLLD | CM_MASH1;
    REG(CM_PCMCTL) = CM_PASSWD | CM_SRC_PLLD | CM_MASH1 | CM_ENAB;
    st.div_q12 = div;
    st.freq = freq;
}

static uint32_t pick_channel(void) {
    // A full (not lite) channel the firmware leaves free, preferring the ones Linux leaves alone.
    static const uint8_t prefer[] = {5, 4, 6, 2};
    uint32_t mask = mbox_dma_channels();
    for (unsigned i = 0; i < sizeof(prefer); i++) {
        if (mask & (1u << prefer[i])) {
            return prefer[i];
        }
    }
    return 5;
}

bool pcm_out_start(const int32_t *ring, uint32_t ring_frames, uint32_t start_frame, uint32_t freq) {
    if (st.running || ring_frames % CHUNKS || (ring_frames & (ring_frames - 1u))) {
        return false;
    }
    if (st.dma_channel == 0xFF) {
        st.dma_channel = pick_channel();
    }
    uint32_t ch = st.dma_channel;

    set_alt0(GPFSEL1, 24);      // GPIO 18 PCM_CLK
    set_alt0(GPFSEL1, 27);      // GPIO 19 PCM_FS
    set_alt0(GPFSEL2, 3);       // GPIO 21 PCM_DOUT
    clock_set(freq);

    // Master clock and frame sync. CLKI and FSI turn the block's native polarity into Philips I2S:
    // data changes on BCK falling, LRCLK low for the left slot. Frame 64 BCK, sync 32.
    REG(PCM_CS) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY;
    REG(PCM_MODE) = (63u << 10) | 32u | (1u << 22) | (1u << 20);
    // Two 32-bit channels (WEX + WID 8), left at BCK 1 and right at BCK 33: one-bit I2S delay.
    REG(PCM_TXC) = (1u << 31) | (1u << 30) | (1u << 20) | (8u << 16) | (1u << 15) | (1u << 14) | (33u << 4) | 8u;
    REG(PCM_RXC) = 0;
    REG(PCM_DREQ) = (0x10u << 24) | (0x30u << 8);   // TX panic 16 words, DREQ below 48 words
    REG(PCM_INTEN) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_TXCLR;
    // SYNC echoes back after two PCM clocks, by when TXCLR has taken effect.
    REG(PCM_CS) = CS_EN | CS_STBY | CS_SYNC;
    uint32_t t0 = micros();
    while (!(REG(PCM_CS) & CS_SYNC) && micros() - t0 < 100u) {
    }
    REG(PCM_CS) = CS_EN | CS_STBY | CS_TXERR;

    uint32_t chunk = ring_frames / CHUNKS;
    uint32_t ti = DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC | DMA_TI_PERMAP_PCM_TX |
                  DMA_TI_NO_WIDE_BURSTS;
    for (uint32_t i = 0; i < CHUNKS; i++) {
        cbs[i] = (dma_cb_t){ti, BUS(ring + 2u * chunk * i), PCM_FIFO_BUS, chunk * 8u, 0,
                            BUS(&cbs[(i + 1u) % CHUNKS]), {0, 0}};
    }
    uint32_t s = start_frame & (ring_frames - 1u);
    uint32_t ci = s / chunk;
    cbs[CHUNKS] = (dma_cb_t){ti, BUS(ring + 2u * s), PCM_FIFO_BUS, ((ci + 1u) * chunk - s) * 8u, 0,
                             BUS(&cbs[(ci + 1u) % CHUNKS]), {0, 0}};
    __asm__ volatile("dsb" ::: "memory");

    REG(DMA_ENABLE) |= 1u << ch;
    REG(DMA_CS(ch)) = DMA_CS_RESET;
    wait_us(2);
    REG(DMA_DEBUG(ch)) = 7u;    // clear read error, FIFO error and read-last-not-set flags
    REG(DMA_CONBLK(ch)) = BUS(&cbs[CHUNKS]);
    REG(DMA_CS(ch)) = DMA_CS_END | DMA_CS_WAIT_WRITES | (8u << 20) | (8u << 16) | DMA_CS_ACTIVE;

    // Let the DMA fill the FIFO, then start shifting out.
    REG(PCM_CS) = CS_EN | CS_STBY | CS_DMAEN;
    wait_us(20);
    REG(PCM_CS) = CS_EN | CS_STBY | CS_DMAEN | CS_TXON;

    ring_bus = BUS(ring);
    ring_mask = ring_frames - 1u;
    st.consumed = start_frame;
    st.running = 1;
    st.starts++;
    last_us = micros();
    return true;
}

void pcm_out_stop(void) {
    if (!st.running) {
        return;
    }
    pcm_out_consumed_frames();
    REG(PCM_CS) = CS_EN | CS_STBY;
    REG(DMA_CS(st.dma_channel)) = DMA_CS_RESET;
    st.running = 0;
}

void pcm_out_set_rate(uint32_t freq) {
    if (st.running) {
        pcm_out_consumed_frames();
    }
    clock_set(freq);
}

uint32_t pcm_out_consumed_frames(void) {
    if (!st.running) {
        return st.consumed;
    }
    uint32_t now = micros();
    uint32_t off = REG(DMA_SOURCE(st.dma_channel)) - ring_bus;
    if (REG(PCM_CS) & CS_TXERR) {
        st.tx_errors++;
        REG(PCM_CS) = CS_EN | CS_STBY | CS_DMAEN | CS_TXON | CS_TXERR;
    }
    if (REG(DMA_DEBUG(st.dma_channel)) & 7u) {
        st.dma_errors++;
        REG(DMA_DEBUG(st.dma_channel)) = 7u;
    }
    if (off > (ring_mask + 1u) * 8u) {
        return st.consumed;     // between control blocks
    }
    // The DMA position gives the count modulo the ring. Add whole rings so the advance is the one
    // nearest what the elapsed time predicts.
    uint32_t n = ring_mask + 1u;
    uint32_t d = ((off / 8u) - st.consumed) & ring_mask;
    uint32_t expect = (uint32_t)((uint64_t)(now - last_us) * st.freq / 1000000u);
    if (expect > d + n / 2u) {
        d += ((expect - d + n / 2u) / n) * n;
    }
    st.consumed += d;
    last_us = now;
    return st.consumed;
}

void pcm_out_status(pcm_out_status_t *out) {
    pcm_out_consumed_frames();
    *out = st;
}
