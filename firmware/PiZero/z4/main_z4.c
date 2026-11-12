// Z4: DSPi on the UAC2 High Speed front end. Every millisecond the rate's worth of frames is taken from
// TinyUSB's FIFO and run through DSPi's process_input_block(); the output rings are drained by the system
// timer (pico_shim.c). Vendor requests go to DSPi's vendor_commands.c, notifications go out on EP 0x83,
// and z4_mainloop.c carries DSPi's main-loop work. Bench counters are read with vendor request 0x4F.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "mmu.h"
#include "irq.h"
#include "mbox.h"
#include "uart.h"

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "broadcom/defines.h"
#include "broadcom/gen/vcmailbox.h"

#include "loader_proto.h"
#include "usb_descriptors.h"

#include "pico/audio_i2s_multi.h"
#include "pico/audio_spdif.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "config.h"
#include "audio_input.h"
#include "audio_pipeline.h"
#include "notify.h"
#include "output_latency.h"
#include "pdm_generator.h"
#include "siggen.h"
#include "usb_audio.h"

#include "z4.h"
#include "z4_globals.h"
#include "pmu.h"
#include "pcm_out.h"
#include "smi_out.h"
#include "z4_prof.h"

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static void usb_irq(void) {
    tud_int_handler(0);
    // Read back from the controller so its interrupt-clear writes have landed before we return.
    // Otherwise the line is still high and the IRQ fires again with nothing pending.
    (void)*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + 0x014);    // GINTSTS
}

// usb_sound_card_init() in DSPi's usb_audio.c calls tud_init(), whose tud_rhport_init() is renamed to
// this for that file only. USB is brought up by main() below instead.
bool dspi_z4_tud_rhport_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
    (void)rhport;
    (void)rh_init;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// State and counters

static const uint32_t sample_rates[] = {44100, 48000, 96000};
#define N_SAMPLE_RATES (sizeof(sample_rates) / sizeof(sample_rates[0]))

static volatile uint32_t current_rate = 48000;
static volatile uint8_t current_alt;
static int8_t mute_master;
static int16_t volume_master;   // 1/256 dB

typedef struct {
    uint32_t class_requests;
    uint32_t set_alt_count;
    uint32_t rate_sets;
    uint32_t reserved0;
    uint32_t fb_sent;
    uint32_t reserved1;
    uint32_t out_packets;
    uint32_t out_bytes;
    uint32_t consumed_bytes;
    uint32_t underruns;     // consumer ticks that found less than a tick's worth in the FIFO
    uint32_t fifo_level;    // bytes in the FIFO at the last consumer tick
    uint32_t suspends;
    uint32_t resumes;
    uint32_t mounts;
    uint32_t irqs;
    uint32_t irq_spurious;
    // Diagnostics for the feedback endpoint (EP 2 IN) and the data endpoint (EP 1 OUT).
    uint32_t fb_diepctl;
    uint32_t fb_dieptsiz;
    uint32_t fb_diepint;
    uint32_t data_doepctl;
    uint32_t gintmsk;
    uint32_t dsts;
    uint32_t fb_busy;
    int32_t fb_parity;
    int32_t data_parity;
} u2_stats_t;

// DSPi side, appended after u2_stats_t in the 0x4F reply (version 2).
#define Z4_PEAK_SLOTS 18
_Static_assert(NUM_CHANNELS <= Z4_PEAK_SLOTS, "peaks[] holds every DSPi channel");

typedef struct {
    uint8_t cpu0_load;              // global_status.cpu0_load: DSPi's busy/budget EMA, percent
    uint8_t cpu1_load;              // always 0, Core 1 is never started
    uint8_t core1_mode;
    uint8_t n_channels;             // valid entries in peaks[] (NUM_CHANNELS)
    uint32_t blocks;                // process_input_block() calls since the stream started
    uint32_t frames;                // frames passed to it
    uint32_t block_us_last;         // duration of the last call
    uint32_t block_us_max;
    uint32_t block_us_avg_x100;     // mean duration, hundredths of a microsecond
    uint32_t clip_flags;            // global_status.clip_flags
    uint16_t peaks[Z4_PEAK_SLOTS];  // global_status.peaks: inputs 0-7, outputs 1-8, PDM; 32767 = full scale
    int32_t slot0_fill;             // output_ring_fill_signed(0), frames
    uint32_t ring_target;           // output_ring_target_frames
    uint32_t ring_drops;            // output_ring_drops: blocks refused by group admission
    uint32_t slot0_underrun_frames; // silence frames skipped into slot 0 by recovery
    uint32_t slot0_drop_frames;     // frames slot 0 refused (should stay 0)
    uint32_t pipeline_rate;         // audio_state.freq
    uint32_t vendor_requests;       // vendor requests DSPi accepted (SETUP stage), version 3
    uint32_t vendor_stalls;         // vendor requests DSPi stalled
    uint32_t notify_packets;        // event packets sent on EP 0x83
    uint32_t notify_keepalives;     // 1-byte idle packets sent on EP 0x83
    // Version 4: performance counters over process_input_block(), all wrapping totals.
    uint32_t pmu_counters;          // event counters the core has, 0 if the counters are unused
    uint32_t perf_blocks;
    uint32_t perf_cycles;           // inside the block, interrupts included
    uint32_t perf_irq_cycles;       // of those, cycles in interrupt handlers (USB and the SMI composer)
    uint32_t perf_irqs;             // interrupts taken inside the block
    uint32_t perf_ev[PMU_N_EVENTS]; // instructions, L1I, L1D and L2 refills, branch mispredicts
    uint32_t irq_cycles_all;        // all interrupt handler cycles
    pcm_out_status_t pcm;           // version 5: the PCM block (Z5 I2S out)
    uint32_t output_clock;          // shim_output_clock(), frames
    smi_out_status_t smi;           // version 7: the SMI output stream
} z4_stats_t;

int8_t dcd_dwc2_iso_parity(uint8_t dir, uint8_t epnum);
#define DWC2_R(off) (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + (off)))

static volatile u2_stats_t stats;
static uint32_t fb_override;    // feedback_task(): bench-fixed feedback value, 0 for automatic
static uint32_t fb_value;

static volatile bool reboot_requested;

static uint32_t dspi_blocks, dspi_frames, dspi_us_last, dspi_us_max;
static uint64_t dspi_us_total;
static uint32_t vendor_requests, vendor_stalls, notify_packets, notify_keepalives;
static uint32_t pmu_counters, perf_blocks, perf_cycles, perf_irq_cycles, perf_irqs, perf_ev[PMU_N_EVENTS];
static volatile bool consumer_running;

void tud_mount_cb(void) {
    stats.mounts++;
    usb_audio_mounted = 1;
}

void tud_umount_cb(void) {
    usb_audio_mounted = 0;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    stats.suspends++;
}

void tud_resume_cb(void) {
    stats.resumes++;
}

// ---------------------------------------------------------------------------------------------------
// UAC2 requests: clock source (rate) and feature unit (master mute and volume)

static bool clock_get(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(current_rate)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) range = {.wNumSubRanges = tu_htole16(N_SAMPLE_RATES)};
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                range.subrange[i].bMin = (int32_t)sample_rates[i];
                range.subrange[i].bMax = (int32_t)sample_rates[i];
                range.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
    } else if (req->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &valid, sizeof(valid));
    }
    return false;
}

static bool clock_set(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bControlSelector != AUDIO_CS_CTRL_SAM_FREQ || req->bRequest != AUDIO_CS_REQ_CUR ||
        req->wLength != sizeof(audio_control_cur_4_t)) {
        return false;
    }
    uint32_t f = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
    for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
        if (f == sample_rates[i]) {
            current_rate = f;
            stats.rate_sets++;
            return true;
        }
    }
    return false;   // unsupported values are stalled, not clamped
}

static bool feature_get(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bChannelNumber != 0) {
        return false;   // master controls only
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t m = {.bCur = mute_master};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &m, sizeof(m));
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            // -60 to 0 dB in 1 dB steps, as on DSPi's UAC1 boards.
            audio_control_range_2_n_t(1) range = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {.bMin = tu_htole16(-60 * 256), .bMax = tu_htole16(0), .bRes = tu_htole16(256)},
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t v = {.bCur = tu_htole16(volume_master)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &v, sizeof(v));
        }
    }
    return false;
}

// Host mute and volume drive DSPi's audio_state the way its UAC1 driver does. These callbacks run
// from tud_task() in the main loop, which audio_set_volume() requires.
static bool feature_set(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bChannelNumber != 0 || req->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->wLength == sizeof(audio_control_cur_1_t)) {
        mute_master = ((audio_control_cur_1_t const *)buf)->bCur;
        audio_state.mute = mute_master != 0;
        return true;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME && req->wLength == sizeof(audio_control_cur_2_t)) {
        volume_master = ((audio_control_cur_2_t const *)buf)->bCur;
        audio_set_volume(volume_master);
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    stats.class_requests++;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_get(rhport, req);
    }
    if (req->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return feature_get(rhport, req);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    stats.class_requests++;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_set(req, buf);
    }
    if (req->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return feature_set(req, buf);
    }
    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING) {
        current_alt = 0;
        usb_audio_alt_set = 0;
    }
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING) {
        current_alt = tu_u16_low(tu_le16toh(p_request->wValue));
        usb_audio_alt_set = current_alt;
        stats.set_alt_count++;
        // A feedback packet still queued when the last stream stopped leaves the endpoint claimed, and
        // the class's re-activation clears busy but not claimed, so feedback would never be sent again.
        if (current_alt != 0) {
            usbd_edpt_release(rhport, EP_AUDIO_FB);
        }
    }
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
    (void)func_id;
    (void)alt_itf;
    // feedback_task() below sets the feedback itself, from how full TinyUSB's FIFO is.
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    feedback_param->sample_freq = current_rate;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    stats.out_packets++;
    stats.out_bytes += n_bytes_received;
    usb_audio_packets++;
    return true;
}

void tud_audio_fb_done_cb(uint8_t func_id) {
    (void)func_id;
    stats.fb_sent++;
}

// ---------------------------------------------------------------------------------------------------
// Vendor interface (DSPi Console's channel) and bench requests

// ---------------------------------------------------------------------------------------------------
// Notifications: usb_notify_drain() from DSPi's usb_audio.c, on this driver's EP 0x83. Events go out
// as soon as they are queued; with none, a 1-byte idle packet after 100 ms of quiet. Each packet is
// at most 64 bytes, a short packet on this 512-byte High Speed bulk endpoint, so each ends a transfer.

#define NOTIFY_IDLE_KEEPALIVE_US 100000u
#define Z4_NOTIFY_MAX_PKT 64u

static uint8_t notify_buf[Z4_NOTIFY_MAX_PKT] __attribute__((aligned(4)));
static bool notify_ep_open;
static uint32_t notify_last_arm_us;

static void notify_drain(uint8_t rhport) {
    if (!notify_ep_open) {
        return;
    }
    if (!notify_has_pending_for(NOTIFY_CONSUMER_USB) &&
        (uint32_t)(time_us_32() - notify_last_arm_us) < NOTIFY_IDLE_KEEPALIVE_US) {
        return;
    }
    if (!usbd_edpt_claim(rhport, EP_NOTIFY_IN)) {
        return;     // busy: the completion re-arms
    }
    uint16_t len = 0;
    if (notify_has_pending_for(NOTIFY_CONSUMER_USB)) {
        len = notify_peek_next_for(NOTIFY_CONSUMER_USB, notify_buf, Z4_NOTIFY_MAX_PKT);
    }
    bool event = len > 0;
    if (!event) {
        notify_buf[0] = 0x00;
        len = 1;
    }
    if (!usbd_edpt_xfer(rhport, EP_NOTIFY_IN, notify_buf, len)) {
        usbd_edpt_release(rhport, EP_NOTIFY_IN);
        return;     // the same event is retried next pass
    }
    notify_last_arm_us = time_us_32();
    if (event) {
        notify_commit_pop_for(NOTIFY_CONSUMER_USB);
        notify_packets++;
    } else {
        notify_keepalives++;
    }
}

static uint16_t vendor_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    uint16_t len = tu_desc_len(itf);
    uint8_t const *p = (uint8_t const *)itf + len;
    for (uint8_t i = 0; i < itf->bNumEndpoints && len < max_len; i++) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            if (ep->bEndpointAddress == EP_NOTIFY_IN) {
                notify_ep_open = true;
            }
        }
        len += tu_desc_len(p);
        p += tu_desc_len(p);
    }
    return len;
}

static bool vendor_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    // Only standard interface requests reach a class driver. The vendor interface has one alt.
    if (stage == CONTROL_STAGE_SETUP && req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        req->bRequest == TUSB_REQ_SET_INTERFACE && req->wValue == 0) {
        return tud_control_status(rhport, req);
    }
    return stage != CONTROL_STAGE_SETUP;
}

static bool vendor_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;
    (void)xferred_bytes;
    if (ep_addr != EP_NOTIFY_IN) {
        return false;
    }
    notify_drain(rhport);   // delivered: send the next one, if any
    return true;
}

static void vendor_noop(void) {}

static bool vendor_deinit(void) {
    return true;
}

// Bus reset, as DSPi's UAC1 driver handles it: queued notifications are stale.
static void vendor_reset(uint8_t rhport) {
    (void)rhport;
    notify_ep_open = false;
    notify_reset_queue();
    notify_master_vol_host_initiated = false;
}

static const usbd_class_driver_t vendor_driver = {
    .name = "DSPi_vendor",
    .init = vendor_noop,
    .deinit = vendor_deinit,
    .reset = vendor_reset,
    .open = vendor_open,
    .control_xfer_cb = vendor_control,
    .xfer_cb = vendor_xfer,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &vendor_driver;
}

#define REQ_Z_GET_STATS 0x4F
#define BOARD_ID_PI_ZERO_2W 1   // GET_PLATFORM byte 7, matched by DSPi Console's Constants.swift
#define U2_STATS_MAGIC  0x5532u     // "U2"

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint32_t uptime_ms;
    uint32_t rate;
    uint8_t alt;
    uint8_t mounted;
    uint8_t speed;      // tusb_speed_t: 0 full, 2 high
    uint8_t reserved;
    u2_stats_t stats;
    z4_stats_t dspi;    // version 2, grown in version 3
} u2_reply_t;

// Wire layout read by the bench tools: version 1 fields unchanged, the DSPi block appended.
_Static_assert(offsetof(u2_reply_t, dspi) == 116, "0x4F reply: DSPi block offset");
_Static_assert(offsetof(z4_stats_t, peaks) == 28 && offsetof(z4_stats_t, slot0_fill) == 64, "0x4F reply layout");
_Static_assert(sizeof(u2_reply_t) == 348, "0x4F reply size");

static void fill_dspi_stats(z4_stats_t *z) {
    memset(z, 0, sizeof(*z));
    z->cpu0_load = global_status.cpu0_load;
    z->cpu1_load = global_status.cpu1_load;
    z->core1_mode = (uint8_t)core1_mode;
    z->n_channels = NUM_CHANNELS;
    z->blocks = dspi_blocks;
    z->frames = dspi_frames;
    z->block_us_last = dspi_us_last;
    z->block_us_max = dspi_us_max;
    z->block_us_avg_x100 = dspi_blocks ? (uint32_t)(dspi_us_total * 100u / dspi_blocks) : 0;
    z->clip_flags = global_status.clip_flags;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        z->peaks[i] = global_status.peaks[i];
    }
    z->slot0_fill = output_ring_fill_signed(0);
    z->ring_target = output_ring_target_frames;
    z->ring_drops = output_ring_drops;
    if (output_types[0] == OUTPUT_TYPE_I2S) {
        audio_i2s_instance_t *inst = i2s_instance_ptrs[0];
        z->slot0_underrun_frames = inst ? inst->underrun_frames : 0;
        z->slot0_drop_frames = inst ? inst->drop_frames : 0;
    } else {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[0];
        z->slot0_underrun_frames = inst ? inst->underrun_frames : 0;
        z->slot0_drop_frames = inst ? inst->drop_frames : 0;
    }
    z->pipeline_rate = audio_state.freq;
    z->vendor_requests = vendor_requests;
    z->vendor_stalls = vendor_stalls;
    z->notify_packets = notify_packets;
    z->notify_keepalives = notify_keepalives;
    z->pmu_counters = pmu_counters;
    z->perf_blocks = perf_blocks;
    z->perf_cycles = perf_cycles;
    z->perf_irq_cycles = perf_irq_cycles;
    z->perf_irqs = perf_irqs;
    memcpy(z->perf_ev, perf_ev, sizeof(perf_ev));
    z->irq_cycles_all = irq_cycles();
    pcm_out_status(&z->pcm);
    z->output_clock = shim_output_clock();
    smi_out_status(&z->smi);
}

bool dspi_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req);

// VideoCore sensor readings for DSPi Console's System Information, refreshed once a second from the
// main loop because a mailbox round trip is too slow for a control request.
static volatile uint32_t sensor_arm_hz, sensor_core_mv;
static volatile int32_t sensor_temp_cc;

static void sensors_refresh(void) {
    static uint32_t last_us;
    static bool primed;
    uint32_t now = board_micros();
    if (primed && now - last_us < 1000000u) {
        return;
    }
    primed = true;
    last_us = now;
    sensor_arm_hz = mbox_clock_rate_measured(VCMAILBOX_CLOCK_ARM);
    sensor_core_mv = (mbox_voltage_uv(1) + 500u) / 1000u;
    sensor_temp_cc = (int32_t)(mbox_temperature_mc() + 5u) / 10;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    static u2_reply_t reply;
    bool in = req->bmRequestType_bit.direction == TUSB_DIR_IN;
    // Bench requests are answered only when sent to the device. DSPi Console and its test harness
    // address the vendor interface, so for them these codes stay unknown and stall, as on an RP2350.
    bool bench = req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE;
    if (bench && req->bRequest == APP_REQ_REBOOT_TO_LOADER && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, req);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reboot_requested = true;
        }
        return true;
    }
    // Profiler: 0x4D starts sampling every wValue us (0 stops). 0x4E reads the sample count with
    // wValue 0xFFFF, otherwise 4 KB of samples at chunk wValue.
    if (bench && req->bRequest == 0x4D && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            z4_prof_start(req->wValue);
            return tud_control_status(rhport, req);
        }
        return true;
    }
    // 0x48: fixes the feedback value at wIndex << 16 | wValue (16.16), or 0 for automatic.
    if (bench && req->bRequest == 0x48 && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            fb_override = (uint32_t)req->wIndex << 16 | req->wValue;
            return tud_control_status(rhport, req);
        }
        return true;
    }
    // 0x49: up to 16 composed SMI frames not yet sent: the first frame's number, then 512 bytes a frame.
    if (bench && req->bRequest == 0x49 && in) {
        static uint32_t snap[1 + 16 * SMI_FRAME_WORDS / 2];
        if (stage == CONTROL_STAGE_SETUP) {
            uint32_t n = req->wValue < 1 ? 1 : (req->wValue > 16 ? 16 : req->wValue);
            snap[0] = shim_smi_snapshot((uint16_t *)&snap[1], n);
            uint32_t len = 4u + n * SMI_FRAME_WORDS * 2u;
            return tud_control_xfer(rhport, req, snap, req->wLength < len ? req->wLength : (uint16_t)len);
        }
        return true;
    }
    // 0x4B: count level changes on GPIO wValue over wIndex ms; replies toggles and samples.
    if (bench && req->bRequest == 0x4B && in) {
        static uint32_t probe_reply[2];
        if (stage == CONTROL_STAGE_SETUP) {
            probe_reply[0] = smi_out_probe_toggles(req->wValue, req->wIndex > 100 ? 100 : req->wIndex,
                                                   &probe_reply[1]);
            return tud_control_xfer(rhport, req, probe_reply, req->wLength < 8 ? req->wLength : 8);
        }
        return true;
    }
    if (bench && req->bRequest == 0x4E && in) {
        static uint32_t count_reply;
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (req->wValue == 0xFFFF) {
            count_reply = z4_prof_count;
            return tud_control_xfer(rhport, req, &count_reply, req->wLength < 4 ? req->wLength : 4);
        }
        uint32_t first = (uint32_t)req->wValue * 1024u;
        if (first >= Z4_PROF_MAX) {
            return false;
        }
        uint16_t len = req->wLength < 4096 ? req->wLength : 4096;
        return tud_control_xfer(rhport, req, &z4_prof_samples[first], len);
    }
    if (bench && req->bRequest == REQ_Z_GET_STATS && in) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        reply.magic = U2_STATS_MAGIC;
        reply.version = 7;
        reply.uptime_ms = board_micros() / 1000u;
        reply.rate = current_rate;
        reply.alt = current_alt;
        reply.mounted = tud_mounted() ? 1u : 0u;
        reply.speed = (uint8_t)tud_speed_get();
        stats.irqs = irq_count();
        stats.irq_spurious = irq_spurious();
        stats.fb_diepctl = DWC2_R(0x900 + 0x20 * 2);
        stats.fb_dieptsiz = DWC2_R(0x910 + 0x20 * 2);
        stats.fb_diepint = DWC2_R(0x908 + 0x20 * 2);
        stats.data_doepctl = DWC2_R(0xB00 + 0x20 * 1);
        stats.gintmsk = DWC2_R(0x018);
        stats.dsts = DWC2_R(0x808);
        stats.fb_busy = usbd_edpt_busy(rhport, EP_AUDIO_FB);
        stats.fb_parity = dcd_dwc2_iso_parity(TUSB_DIR_IN, 2);
        stats.data_parity = dcd_dwc2_iso_parity(TUSB_DIR_OUT, 1);
        memcpy(&reply.stats, (const void *)&stats, sizeof(reply.stats));
        fill_dspi_stats(&reply.dspi);
        uint16_t len = req->wLength < sizeof(reply) ? req->wLength : (uint16_t)sizeof(reply);
        return tud_control_xfer(rhport, req, &reply, len);
    }
    // GET_PLATFORM for 8 or more bytes: DSPi's 7-byte reply plus a board byte that DSPi Console shows
    // in Device Information. Byte 0 stays RP2350, the code base, which every feature gate uses, so a
    // Console that asks for 7 bytes (or predates the board byte) sees an RP2350 as before.
    if (!bench && req->bRequest == REQ_GET_PLATFORM && in && req->wLength >= 8) {
        static uint8_t platform_reply[8];
        if (stage == CONTROL_STAGE_SETUP) {
            platform_reply[0] = PLATFORM_RP2350;
            platform_reply[1] = (uint8_t)(FW_VERSION_PACKED >> 8);
            platform_reply[2] = (uint8_t)(FW_VERSION_PACKED & 0xFF);
            platform_reply[3] = NUM_OUTPUT_CHANNELS;
            platform_reply[4] = FW_VERSION_MINOR;
            platform_reply[5] = FW_VERSION_PATCH;
            platform_reply[6] = FW_VERSION_BETA;
            platform_reply[7] = BOARD_ID_PI_ZERO_2W;
            vendor_requests++;
            return tud_control_xfer(rhport, req, platform_reply, sizeof(platform_reply));
        }
        return true;
    }
    // GET_STATUS 13, 14 and 16 (clock, core voltage, temperature) come from the VideoCore, not DSPi's
    // RP2350 clock tree, regulator table and ADC formula. Values are cached by sensors_refresh().
    if (!bench && req->bRequest == REQ_GET_STATUS && in &&
        (req->wValue == 13 || req->wValue == 14 || req->wValue == 16)) {
        static uint32_t sensor_reply;
        if (stage == CONTROL_STAGE_SETUP) {
            sensor_reply = req->wValue == 13 ? sensor_arm_hz : req->wValue == 14 ? sensor_core_mv : (uint32_t)sensor_temp_cc;
            vendor_requests++;
            uint16_t len = req->wLength < 4 ? req->wLength : 4;
            return tud_control_xfer(rhport, req, &sensor_reply, len);
        }
        return true;
    }
    // Everything else is DSPi's, every stage.
    bool ok = dspi_vendor_control_xfer_cb(rhport, stage, req);
    if (stage == CONTROL_STAGE_SETUP) {
        if (ok) {
            vendor_requests++;
        } else {
            vendor_stalls++;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// Stream consumer: every millisecond, the sample rate's worth of frames from the FIFO into DSPi.
// It starts once the FIFO is half full, the level FIFO-count feedback aims for.

static uint8_t rx_buf[AUDIO_BUFFER_SAMPLES * 6] __attribute__((aligned(4)));

// DSPi's own USB decode (process_audio_packet in usb_audio.c, RP2350 stereo path): full-scale integer
// to +-1.0, times the per-channel preamp.
static void decode_into_dspi(uint32_t frames, uint8_t alt) {
    const float gain_l = global_preamp_linear[0];
    const float gain_r = global_preamp_linear[1];
    if (alt == 1) {
        const int16_t *in = (const int16_t *)rx_buf;
        const float g_l = (1.0f / 32768.0f) * gain_l;
        const float g_r = (1.0f / 32768.0f) * gain_r;
        for (uint32_t i = 0; i < frames; i++) {
            buf_l[i] = (float)in[2 * i] * g_l;
            buf_r[i] = (float)in[2 * i + 1] * g_r;
        }
    } else {
        const uint8_t *p = rx_buf;
        const float g_l = (1.0f / 8388608.0f) * gain_l;
        const float g_r = (1.0f / 8388608.0f) * gain_r;
        for (uint32_t i = 0; i < frames; i++, p += 6) {
            int32_t l = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            int32_t r = (int32_t)((uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24) >> 8;
            buf_l[i] = (float)l * g_l;
            buf_r[i] = (float)r * g_r;
        }
    }
}

static void run_block(uint32_t frames, uint8_t alt) {
    decode_into_dspi(frames, alt);
    if (!sync_started) {
        start_time_us = time_us_64();
        sync_started = true;
    }
    total_samples_produced += frames;

    pmu_snap_t a, b;
    uint32_t irq_c0 = irq_cycles(), irq_n0 = irq_count();
    uint32_t t0 = board_micros();
    pmu_read(&a);
    z4_prof_in_block = true;
    process_input_block(frames);
    z4_prof_in_block = false;
    pmu_read(&b);
    uint32_t us = board_micros() - t0;
    perf_blocks++;
    perf_cycles += b.cycles - a.cycles;
    perf_irq_cycles += irq_cycles() - irq_c0;
    perf_irqs += irq_count() - irq_n0;
    for (int i = 0; i < PMU_N_EVENTS; i++) {
        perf_ev[i] += b.ev[i] - a.ev[i];
    }

    dspi_blocks++;
    dspi_frames += frames;
    dspi_us_last = us;
    dspi_us_total += us;
    if (us > dspi_us_max) {
        dspi_us_max = us;
    }
}

// A starting stream only restarts the pipeline while USB is the selected input.
static void stream_start(void) {
    sync_started = false;
    total_samples_produced = 0;
    if (active_input_source == INPUT_SOURCE_USB) {
        z4_stream_restart();
    }
    dspi_blocks = dspi_frames = dspi_us_last = dspi_us_max = 0;
    dspi_us_total = 0;
}

// Feedback in 16.16 samples per data-endpoint service interval (2^(bInterval-1) microframes, 1 ms here),
// which is how macOS reads it: a per-microframe value (TinyUSB's FIFO-count method) is out of range and
// ignored, so the host runs at nominal. Bring-up doc section 19 has the measurement. The value is
// nominal plus a proportional term on the smoothed FIFO level. Bench request 0x48 can fix it.
#define FB_UFRAMES (1u << (UAC2_DATA_INTERVAL - 1u))
#define FB_GAIN 6                   // 16.16 units per byte below half full: 64 ppm costs about 35 bytes

static void feedback_task(uint16_t level) {
    static uint32_t avg_q8;     // FIFO level, bytes, 8 fractional bits
    uint32_t half = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2;
    if (avg_q8 == 0) {
        avg_q8 = half << 8;
    }
    avg_q8 = avg_q8 - (avg_q8 >> 6) + ((uint32_t)level << 2);     // about 64 ticks
    uint32_t nominal = (uint32_t)(((uint64_t)current_rate << 16) * FB_UFRAMES / 8000u);
    int32_t fb = (int32_t)nominal + ((int32_t)(half << 8) - (int32_t)avg_q8) * FB_GAIN / 256;
    int32_t span = (int32_t)(nominal / 100u);   // at most 1% either way
    if (fb > (int32_t)nominal + span) {
        fb = (int32_t)nominal + span;
    } else if (fb < (int32_t)nominal - span) {
        fb = (int32_t)nominal - span;
    }
    fb_value = fb_override ? fb_override : (uint32_t)fb;
    tud_audio_fb_set(fb_value);
}

// While another input is selected the host's stream is drained and dropped, as DSPi ignores it.
// Each 1 ms tick takes as many frames as the output clock advanced: the DAC's own rate while the PCM
// block plays, so the FIFO level, and with it the host's feedback, follows the DAC.
static void consume_task(void) {
    static uint32_t last_us;
    static uint32_t last_clk;   // shim_output_clock() at the last tick
    static bool running;
    static bool busy;

    if (busy) {
        return;     // a stream restart is settling the pipeline from in here
    }
    uint32_t now = board_micros();
    if (now - last_us < 1000u) {
        return;
    }
    last_us = now;
    uint32_t clk = shim_output_clock();
    uint32_t frames = clk - last_clk;
    last_clk = clk;
    if (frames > current_rate / 100u) {
        frames = current_rate / 1000u;  // first tick, or the loop was held up: do not try to catch up
    }

    uint8_t alt = current_alt;
    uint16_t level = tud_audio_available();
    stats.fifo_level = level;
    if (current_alt != 0) {
        feedback_task(level);
    }
    bool usb_source = active_input_source == INPUT_SOURCE_USB;
    if (alt == 0) {
        if (running) {
            sync_started = false;
        }
        running = false;
        consumer_running = false;
        return;
    }
    if (!running) {
        if (level < CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) {
            return;
        }
        running = true;
        busy = true;
        stream_start();
        busy = false;
        last_us = board_micros();
        last_clk = shim_output_clock();
        frames = current_rate / 1000u;
    } else if (usb_source && audio_state.freq != current_rate) {
        busy = true;
        stream_start();     // the host changed rate without closing the stream
        busy = false;
        last_us = board_micros();
        last_clk = shim_output_clock();
        frames = current_rate / 1000u;
    }
    consumer_running = usb_source;

    uint32_t bytes_per_frame = 2u * (alt == 1 ? 2u : 3u);
    // Nothing else steers the slot fill (on the RP2350, DSPi's feedback does), so nudge it towards DSPi's
    // target by up to 2 frames a tick. The FIFO absorbs the difference and the feedback re-centres it.
    // The fill is read before this tick's write, so half the write is added to servo the mid-block fill.
    if (usb_source) {
        int32_t mid = output_ring_fill_signed(0) + (int32_t)frames / 2;
        int32_t corr = ((int32_t)output_ring_target_frames - mid) / 16;
        corr = corr > 2 ? 2 : (corr < -2 ? -2 : corr);
        frames = (int32_t)frames + corr > 0 ? (uint32_t)((int32_t)frames + corr) : 0u;
    }
    uint32_t avail = level / bytes_per_frame;
    if (avail < frames) {
        stats.underruns++;
        frames = avail;
    }
    // At most one producer block per pipeline call (output_latency.c's cap, 192 at most).
    while (frames) {
        uint32_t n = frames < output_ring_block_cap ? frames : output_ring_block_cap;
        if (n > AUDIO_BUFFER_SAMPLES) {
            n = AUDIO_BUFFER_SAMPLES;
        }
        uint16_t got = tud_audio_read(rx_buf, (uint16_t)(n * bytes_per_frame));
        stats.consumed_bytes += got;
        n = got / bytes_per_frame;
        if (n == 0) {
            break;
        }
        if (usb_source) {
            run_block(n, alt);
        }
        frames -= n;
    }
}

// ---------------------------------------------------------------------------------------------------
// Hooks for DSPi and z4_mainloop.c

void z4_service_producer(void) {
    tud_task();
    consume_task();
    input_delivery_mark();
    siggen_pump();
    output_rings_idle_pump();
    notify_drain(0);
}

bool z4_stream_running(void) {
    return consumer_running;
}

uint32_t z4_host_rate(void) {
    return current_rate;
}

// Replaces usb_audio.c's (renamed there): DSPi's version times UAC1 packets, which never arrive here.
// The test-signal pump asks it whether USB is already producing blocks.
bool usb_audio_stream_active(void) {
    return consumer_running && sync_started;
}

// ---------------------------------------------------------------------------------------------------

static void set_serial_string(void) {
    uint64_t serial = mbox_board_serial();
    if (serial == 0) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t nib = (uint32_t)(serial >> (60 - 4 * i)) & 0xFu;
        usb_serial_string[i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    // DSPi's own copy, which its vendor requests report (RP2350: the chip ID).
    memcpy(usb_descriptor_str_serial, usb_serial_string, sizeof(usb_descriptor_str_serial));
}

int main(void) {
#if BCM_VERSION == 2837
    mmu_enable();   // D-cache on: DSP data no longer comes from SDRAM on every access
    pmu_counters = pmu_init();
#endif
    // The VideoCore keeps the ARM at arm_freq_min (600 MHz on a Zero 2 W) until the OS asks for more,
    // as Linux's cpufreq does. Ask for the maximum; the firmware still throttles if it gets too hot.
    {
        uint32_t max_hz = mbox_clock_rate_max(VCMAILBOX_CLOCK_ARM);
        if (max_hz) {
            mbox_set_clock_rate(VCMAILBOX_CLOCK_ARM, max_hz);
        }
    }
    board_led_init();
    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    uart_puts("\nDSPi PiZero Z4 pipeline (" __DATE__ " " __TIME__ ")\n");
    set_serial_string();
    mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);
    uart_set_buffered(1);

    // Outputs first, with interrupts on for its composer, so DSPi's slots start on the SMI's frame clock.
    shim_smi_start();
    irq_global_enable();
    z4_dspi_init();

    irq_set_handler(IRQ_USB, usb_irq);
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    tusb_init(0, &dev_init);    // enables the USB IRQ in the interrupt controller
    irq_global_enable();

    uint32_t last_toggle = board_micros();
    int led_on = 1;
    for (;;) {
        sensors_refresh();
        z4_service_producer();
        z4_dspi_service();
        uart_poll();

        if (reboot_requested) {
            board_delay_us(20000);
            irq_global_disable();
            tud_disconnect();
            board_delay_us(50000);
            board_reboot();
        }

        uint32_t now = board_micros();
        if (now - last_toggle >= (current_alt ? 125000u : 500000u)) {
            last_toggle = now;
            led_on = !led_on;
            board_led_write(led_on);
        }
    }
}
