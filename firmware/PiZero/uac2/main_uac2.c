// UAC2 at High Speed with TinyUSB's audio class, interrupt-driven. Received audio goes into the class
// FIFO and a timer-paced consumer takes it out at the sample rate, standing in for the audio output, so
// TinyUSB's FIFO-count feedback has something real to track. Counters are read with vendor request 0x4F
// (tools/uac2_stream_check.py) and request 0x40 reboots to the USB loader.
// LED: 1 Hz idle, 4 Hz while streaming.

#include <stdbool.h>
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

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static void usb_irq(void) {
    tud_int_handler(0);
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

int8_t dcd_dwc2_iso_parity(uint8_t dir, uint8_t epnum);
#define DWC2_R(off) (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + (off)))

static volatile u2_stats_t stats;
static volatile bool reboot_requested;

void tud_mount_cb(void) {
    stats.mounts++;
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

static bool feature_set(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bChannelNumber != 0 || req->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->wLength == sizeof(audio_control_cur_1_t)) {
        mute_master = ((audio_control_cur_1_t const *)buf)->bCur;
        return true;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME && req->wLength == sizeof(audio_control_cur_2_t)) {
        volume_master = ((audio_control_cur_2_t const *)buf)->bCur;
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
    }
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING) {
        current_alt = tu_u16_low(tu_le16toh(p_request->wValue));
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
} u2_reply_t;

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
        reply.version = 1;
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
        uint16_t len = req->wLength < sizeof(reply) ? req->wLength : (uint16_t)sizeof(reply);
        return tud_control_xfer(rhport, req, &reply, len);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Timer-paced consumer: takes the sample rate's worth of frames out of the FIFO every millisecond,
// as an audio output would. It starts once the FIFO is half full, the level FIFO-count feedback aims for.

static uint8_t sink[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ];

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
        running = false;
        frac = 0;
        return;
    }
    if (!running) {
        if (level < CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) {
            return;
        }
        running = true;
    }

    uint32_t bytes_per_frame = 2u * (alt == 1 ? 2u : 3u);
    frac += current_rate * elapsed;
    uint32_t frames = frac / 1000000u;
    frac -= frames * 1000000u;
    uint32_t want = frames * bytes_per_frame;
    if (want > sizeof(sink)) {
        want = sizeof(sink);
    }
    if (level < want) {
        stats.underruns++;
        want = level - level % bytes_per_frame;
    }
    if (want) {
        stats.consumed_bytes += tud_audio_read(sink, (uint16_t)want);
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
    uart_puts("\nDSPi PiZero UAC2 High Speed (" __DATE__ " " __TIME__ ")\n");
    set_serial_string();
    mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);
    uart_set_buffered(1);

    irq_set_handler(IRQ_USB, usb_irq);
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    tusb_init(0, &dev_init);    // enables the USB IRQ in the interrupt controller
    irq_global_enable();

    uint32_t last_toggle = board_micros();
    int led_on = 1;
    for (;;) {
        tud_task();
        consume_task();
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
