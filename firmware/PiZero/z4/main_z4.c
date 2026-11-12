// Z4a: DSPi's RP2350 float pipeline running on audio received by the UAC2 High Speed front end.
// Every millisecond the rate's worth of frames is taken from TinyUSB's FIFO, decoded into DSPi's input
// buffers and run through process_input_block(). The output slot rings are drained by the system timer
// (pico_shim.c), standing in for the outputs. Counters are read with vendor request 0x4F.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
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
#include "adat_output.h"
#include "audio_input.h"
#include "audio_pipeline.h"
#include "crossfeed.h"
#include "dsp_pipeline.h"
#include "flash_storage.h"
#include "leveller.h"
#include "limiter.h"
#include "loudness.h"
#include "output_latency.h"
#include "pdm_generator.h"
#include "psybass.h"
#include "rta.h"
#include "subharm.h"
#include "tube.h"
#include "upmix.h"
#include "usb_audio.h"

#include "z4_globals.h"

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static void usb_irq(void) {
    tud_int_handler(0);
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
} z4_stats_t;

int8_t dcd_dwc2_iso_parity(uint8_t dir, uint8_t epnum);
#define DWC2_R(off) (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + (off)))

static volatile u2_stats_t stats;
static volatile bool reboot_requested;

static uint32_t dspi_blocks, dspi_frames, dspi_us_last, dspi_us_max;
static uint64_t dspi_us_total;

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
    // TinyUSB derives the feedback from how full its FIFO is, which the paced consumer below drains.
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
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

static uint16_t vendor_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    uint16_t len = tu_desc_len(itf);
    uint8_t const *p = (uint8_t const *)itf + len;
    for (uint8_t i = 0; i < itf->bNumEndpoints && len < max_len; i++) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p), 0);
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
    (void)rhport;
    (void)result;
    (void)xferred_bytes;
    return ep_addr == EP_NOTIFY_IN;
}

static void vendor_noop(void) {}

static bool vendor_deinit(void) {
    return true;
}

static void vendor_reset(uint8_t rhport) {
    (void)rhport;
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
    z4_stats_t dspi;    // version 2
} u2_reply_t;

// Wire layout read by the bench tools: version 1 fields unchanged, the DSPi block appended.
_Static_assert(offsetof(u2_reply_t, dspi) == 116, "0x4F reply: DSPi block offset");
_Static_assert(offsetof(z4_stats_t, peaks) == 28 && offsetof(z4_stats_t, slot0_fill) == 64, "0x4F reply layout");
_Static_assert(sizeof(u2_reply_t) == 204, "0x4F reply size");

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
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    static u2_reply_t reply;
    bool in = req->bmRequestType_bit.direction == TUSB_DIR_IN;
    if (req->bRequest == APP_REQ_REBOOT_TO_LOADER && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, req);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reboot_requested = true;
        }
        return true;
    }
    if (req->bRequest == REQ_Z_GET_STATS && in) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        reply.magic = U2_STATS_MAGIC;
        reply.version = 2;
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
    return false;
}

// ---------------------------------------------------------------------------------------------------
// DSPi bring-up and the deferred work its main loop would do

// Stop every set-up slot, reset its ring and start them all together, then hold the pipeline muted
// until the rings have refilled, as main.c's complete_pipeline_reset() does for USB.
static void restart_outputs(void) {
    audio_spdif_instance_t *spdif_sync[NUM_SPDIF_INSTANCES];
    audio_i2s_instance_t *i2s_sync[NUM_SPDIF_INSTANCES];
    uint spdif_count = 0, i2s_count = 0;
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            audio_i2s_instance_t *inst = i2s_instance_ptrs[i];
            if (inst && inst->ring) {
                audio_i2s_set_enabled(inst, false);
                audio_i2s_ring_reset(inst);
                i2s_sync[i2s_count++] = inst;
            }
        } else {
            audio_spdif_instance_t *inst = spdif_instance_ptrs[i];
            if (inst && inst->ring) {
                audio_spdif_set_enabled(inst, false);
                audio_spdif_ring_reset(inst);
                spdif_sync[spdif_count++] = inst;
            }
        }
    }
    uint32_t flags = save_and_disable_interrupts();
    audio_spdif_enable_sync(spdif_sync, spdif_count);
    audio_i2s_enable_sync(i2s_sync, i2s_count);
    restore_interrupts(flags);
    pipeline_request_soft_mute(output_ring_target_frames + PRESET_MUTE_SAMPLES);
}

// The parts of main.c's perform_rate_change() that apply without output hardware. The coefficient
// recomputes it defers are picked up by service_deferred() on the next pass.
static void apply_rate(uint32_t rate) {
    output_rate_hz = rate;
    output_latency_apply(rate, active_input_source);
    audio_state.freq = rate;
    rta_restart();
    sync_started = false;
    total_samples_produced = 0;
    nominal_feedback_10_14 = (uint32_t)(((uint64_t)rate << 14) / 1000u);
    feedback_10_14 = nominal_feedback_10_14;

    uint32_t flags = save_and_disable_interrupts();
    dsp_recalculate_all_filters((float)rate);
    restore_interrupts(flags);
    loudness_recompute_pending = true;
    crossfeed_update_pending = true;
    leveller_update_pending = true;
    psybass_update_pending = true;
    subharm_update_pending = true;
    tube_update_pending = true;
    limiter_update_pending = true;
    upmix_update_pending = true;
    pdm_update_clock(rate);
    adat_output_on_rate_change(rate);
    audio_i2s_update_all_frequencies(rate);
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[i];
        if (output_types[i] != OUTPUT_TYPE_I2S && inst && inst->ring) {
            audio_spdif_apply_pio_frequency(inst, rate);
        }
    }
    restart_outputs();
}

// main.c's coefficient-update handlers for the flags a rate change sets. Loudness is rebuilt in one go
// (as core0_init does) rather than as main.c's background job. Nothing else sets these flags yet:
// the vendor command path, and with it eq_update_pending, is not part of this image.
static void service_deferred(void) {
    if (loudness_recompute_pending) {
        loudness_recompute_pending = false;
        loudness_recompute_table(loudness_ref_spl, loudness_intensity_pct, (float)audio_state.freq);
        if (loudness_enabled && loudness_active_table) {
            uint8_t idx = effective_vol_index;
            if (idx > CENTER_VOLUME_INDEX) {
                idx = CENTER_VOLUME_INDEX;
            }
            current_loudness_coeffs = loudness_active_table[idx];
        }
    }
    if (crossfeed_update_pending) {
        crossfeed_update_pending = false;
        crossfeed_apply_config((const CrossfeedConfig *)&crossfeed_config, (float)audio_state.freq);
    }
    if (psybass_update_pending) {
        psybass_update_pending = false;
        psybass_apply_config((const PsybassConfig *)&psybass_config, (float)audio_state.freq);
    }
    if (subharm_update_pending) {
        subharm_update_pending = false;
        subharm_apply_config((const SubharmConfig *)&subharm_config, (float)audio_state.freq);
    }
    if (tube_update_pending) {
        tube_update_pending = false;
        tube_apply_config((const TubeConfig *)&tube_config, (float)audio_state.freq);
    }
    if (limiter_update_pending) {
        limiter_update_pending = false;
        limiter_apply_config((float)audio_state.freq);
    }
    if (upmix_update_pending) {
        upmix_update_pending = false;
        upmix_apply_config((const UpmixConfig *)&upmix_config, (float)audio_state.freq);
    }
    if (leveller_update_pending) {
        leveller_update_pending = false;
        leveller_compute_coefficients(&leveller_coeffs, (const LevellerConfig *)&leveller_config,
                                      (float)audio_state.freq);
        if (leveller_reset_pending) {
            leveller_reset_pending = false;
            leveller_reset_state(&leveller_state);
        }
        leveller_bypassed = !leveller_config.enabled;
    }
    // The Core 1 EQ worker does not exist here, so the pipeline must never dispatch to it.
    if (core1_mode != CORE1_MODE_IDLE) {
        core1_mode = CORE1_MODE_IDLE;
    }
}

// main.c's core0_init() in the same order, keeping what applies without RP2350 hardware or flash.
// Skipped: clocks and regulator, soft VCXO, stall alarm, clock diagnostics, preset load (factory
// defaults stay in force), DAC mute, PDM hardware and Core 1, S/PDIF and I2S receivers, LG Sound Sync,
// notifications, UART/I2C control and Control Surfaces.
static void dspi_init(void) {
    output_latency_apply(48000u, INPUT_SOURCE_USB);
    usb_sound_card_init();
    adat_output_init();

    nominal_feedback_10_14 = (uint32_t)(((uint64_t)audio_state.freq << 14) / 1000u);
    feedback_10_14 = nominal_feedback_10_14;
    output_latency_apply(48000u, active_input_source);

    uint32_t flags = save_and_disable_interrupts();
    dsp_recalculate_all_filters(48000.0f);
    restore_interrupts(flags);

    loudness_recompute_table(loudness_ref_spl, loudness_intensity_pct, 48000.0f);
    if (loudness_enabled && loudness_active_table) {
        current_loudness_coeffs = loudness_active_table[effective_vol_index];
    }
    leveller_compute_coefficients(&leveller_coeffs, (const LevellerConfig *)&leveller_config, 48000.0f);
    leveller_reset_state(&leveller_state);
    leveller_bypassed = !leveller_config.enabled;
    psybass_apply_config((const PsybassConfig *)&psybass_config, 48000.0f);
    subharm_apply_config((const SubharmConfig *)&subharm_config, 48000.0f);
    tube_apply_config((const TubeConfig *)&tube_config, 48000.0f);
    limiter_apply_config(48000.0f);
    upmix_apply_config((const UpmixConfig *)&upmix_config, 48000.0f);

    // Single core: Core 1 stays idle whatever the outputs say, and PDM is off.
    core1_mode = CORE1_MODE_IDLE;
    pdm_set_enabled(false);
    rta_init();

    // usb_sound_card_init() queues a rate change for main.c's loop. Here the rate comes from the UAC2
    // clock (48 kHz until the host sets it), and a stream start applies whatever the host chose.
    rate_change_pending = false;
    apply_rate(current_rate);
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
    input_delivery_mark();

    uint32_t t0 = board_micros();
    process_input_block(frames);
    uint32_t us = board_micros() - t0;

    dspi_blocks++;
    dspi_frames += frames;
    dspi_us_last = us;
    dspi_us_total += us;
    if (us > dspi_us_max) {
        dspi_us_max = us;
    }
}

static void stream_start(void) {
    if (audio_state.freq != current_rate) {
        apply_rate(current_rate);
    } else {
        restart_outputs();
    }
    sync_started = false;
    total_samples_produced = 0;
    pipeline_reset_cpu_metering();
    dspi_blocks = dspi_frames = dspi_us_last = dspi_us_max = 0;
    dspi_us_total = 0;
}

static void consume_task(void) {
    static uint32_t last_us;
    static uint32_t frac;       // frames * 1e6, carried between ticks for 44.1 kHz
    static bool running;

    uint32_t now = board_micros();
    if (now - last_us < 1000u) {
        return;
    }
    uint32_t elapsed = now - last_us;
    last_us = now;
    if (elapsed > 10000u) {
        elapsed = 1000u;    // first tick, or the loop was held up: do not try to catch up
    }

    uint8_t alt = current_alt;
    uint16_t level = tud_audio_available();
    stats.fifo_level = level;
    if (alt == 0) {
        if (running) {
            sync_started = false;
        }
        running = false;
        frac = 0;
        return;
    }
    if (!running) {
        if (level < CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) {
            return;
        }
        running = true;
        stream_start();
        last_us = board_micros();
        elapsed = 1000u;
    } else if (audio_state.freq != current_rate) {
        stream_start();     // the host changed rate without closing the stream
        last_us = board_micros();
        elapsed = 1000u;
        frac = 0;
    }

    uint32_t bytes_per_frame = 2u * (alt == 1 ? 2u : 3u);
    frac += current_rate * elapsed;
    uint32_t frames = frac / 1000000u;
    frac -= frames * 1000000u;
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
        run_block(n, alt);
        frames -= n;
    }
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
}

int main(void) {
    board_led_init();
    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    uart_puts("\nDSPi PiZero Z4 pipeline (" __DATE__ " " __TIME__ ")\n");
    set_serial_string();
    mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);
    uart_set_buffered(1);

    dspi_init();
    service_deferred();

    irq_set_handler(IRQ_USB, usb_irq);
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    tusb_init(0, &dev_init);    // enables the USB IRQ in the interrupt controller
    irq_global_enable();

    uint32_t last_toggle = board_micros();
    int led_on = 1;
    for (;;) {
        tud_task();
        consume_task();
        output_rings_idle_pump();
        service_deferred();
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
