// Milestone Z2: DSPi's UAC1 class driver cut down to enumeration. It claims the AC + AS audio function
// and the vendor interface from DSPi's own descriptors (firmware/DSPi/usb_descriptors.c, stereo alts
// 1 and 2), answers the UAC1 mute, volume and sampling-rate requests, and returns a fixed nominal
// feedback value. Adapted from firmware/DSPi/usb_audio.c. Received audio is counted and dropped.

#include <string.h>

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "usb_descriptors.h"
#include "uac1_z2.h"

#define ENCODE_DB(x)        ((int16_t)((x) * 256))
#define MIN_VOLUME          ENCODE_DB(-60)      // DSPi's CENTER_VOLUME_INDEX
#define MAX_VOLUME          ENCODE_DB(0)
#define VOLUME_RESOLUTION   ENCODE_DB(1)

static uint8_t ep_out_buf[AUDIO_EP_MAX_PKT] __attribute__((aligned(4)));
static uint8_t ep_fb_buf[4] __attribute__((aligned(4)));
static uint8_t uac1_ctrl_buf[8];

static struct {
    uint8_t ac_itf;
    uint8_t as_itf;
    uint8_t vendor_itf;     // 0xFF = not claimed
    uint8_t cur_alt;
    bool    ep_data_open;
    bool    ep_fb_open;
    bool    notify_ep_open;
    bool    eps_activated;  // iso EPs set up in the controller since the last bus reset
    uint8_t pending_cs;
    uint8_t pending_recipient;
} uac1 = { .vendor_itf = 0xFF };

static uint32_t selected_rate = 48000;
static uint32_t sof_frame;          // last SOF frame number (11 bits)
static uint32_t fb_armed_frame;     // SOF frame at which the pending feedback packet was queued
static bool     mute;
static int16_t  volume = MAX_VOLUME;

volatile uac1_z2_stats_t uac1_z2_stats;

static uint32_t nominal_feedback(void) {
    return (uint32_t)(((uint64_t)selected_rate << 14) / 1000u);
}

uint32_t uac1_z2_rate(void) {
    return selected_rate;
}

uint8_t uac1_z2_alt(void) {
    return uac1.cur_alt;
}

static void uac1_driver_init(void) {
    memset(&uac1, 0, sizeof(uac1));
    uac1.vendor_itf = 0xFF;
}

static bool uac1_driver_deinit(void) {
    return true;
}

static void uac1_driver_reset(uint8_t rhport) {
    (void)rhport;
    uac1.ep_data_open = false;
    uac1.ep_fb_open = false;
    uac1.notify_ep_open = false;
    uac1.eps_activated = false;
    uac1.cur_alt = 0;
    uac1.vendor_itf = 0xFF;
}

static uint16_t uac1_driver_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    // TinyUSB calls open() a second time for the vendor interface, which owns the notification EP 0x83.
    if (itf_desc->bInterfaceClass == 0xFF && itf_desc->bAlternateSetting == 0 && itf_desc->bNumEndpoints <= 1) {
        uac1.vendor_itf = itf_desc->bInterfaceNumber;
        uint16_t drv_len = tu_desc_len(itf_desc);
        if (itf_desc->bNumEndpoints == 1) {
            uint8_t const *ep_desc = (uint8_t const *)itf_desc + tu_desc_len(itf_desc);
            if (tu_desc_type(ep_desc) == TUSB_DESC_ENDPOINT) {
                TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)ep_desc));
                uac1.notify_ep_open = true;
                drv_len += tu_desc_len(ep_desc);
            }
        }
        return drv_len;
    }

    TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_AUDIO);
    TU_VERIFY(itf_desc->bInterfaceSubClass == AUDIO_SUBCLASS_CONTROL);
    TU_VERIFY(itf_desc->bAlternateSetting == 0);
    uac1.ac_itf = itf_desc->bInterfaceNumber;

    uint8_t const *p_desc = (uint8_t const *)itf_desc;
    uint8_t const *p_end = p_desc + max_len;
    uint16_t drv_len = tu_desc_len(p_desc);
    p_desc += tu_desc_len(p_desc);

    while (p_desc < p_end && tu_desc_type(p_desc) == TUSB_DESC_CS_INTERFACE) {
        drv_len += tu_desc_len(p_desc);
        p_desc += tu_desc_len(p_desc);
    }

    // Reserve FIFO space for the largest data OUT packet and the 3-byte feedback IN, once.
    bool allocated_out = false;
    bool allocated_fb = false;
    while (p_desc < p_end && tu_desc_type(p_desc) == TUSB_DESC_INTERFACE) {
        tusb_desc_interface_t const *as = (tusb_desc_interface_t const *)p_desc;
        if (as->bInterfaceClass != TUSB_CLASS_AUDIO || as->bInterfaceSubClass != AUDIO_SUBCLASS_STREAMING) {
            break;
        }
        uac1.as_itf = as->bInterfaceNumber;
        drv_len += tu_desc_len(p_desc);
        p_desc += tu_desc_len(p_desc);

        while (p_desc < p_end && tu_desc_type(p_desc) != TUSB_DESC_INTERFACE) {
            if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
                tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
                if (ep->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS) {
                    if (ep->bEndpointAddress == AUDIO_OUT_ENDPOINT && !allocated_out) {
                        TU_ASSERT(usbd_edpt_iso_alloc(rhport, AUDIO_OUT_ENDPOINT, AUDIO_EP_MAX_PKT));
                        allocated_out = true;
                    } else if (ep->bEndpointAddress == AUDIO_IN_ENDPOINT && !allocated_fb) {
                        TU_ASSERT(usbd_edpt_iso_alloc(rhport, AUDIO_IN_ENDPOINT, 4));
                        allocated_fb = true;
                    }
                }
            }
            drv_len += tu_desc_len(p_desc);
            p_desc += tu_desc_len(p_desc);
        }
    }

    usbd_sof_enable(rhport, SOF_CONSUMER_AUDIO, true);
    return drv_len;
}

static void arm_data_out(uint8_t rhport) {
    usbd_edpt_xfer(rhport, AUDIO_OUT_ENDPOINT, ep_out_buf, AUDIO_EP_MAX_PKT);
}

static void arm_feedback(uint8_t rhport) {
    uint32_t fb = nominal_feedback();
    ep_fb_buf[0] = (uint8_t)fb;
    ep_fb_buf[1] = (uint8_t)(fb >> 8);
    ep_fb_buf[2] = (uint8_t)(fb >> 16);
    ep_fb_buf[3] = 0;
    if (usbd_edpt_xfer(rhport, AUDIO_IN_ENDPOINT, ep_fb_buf, 3)) {
        uac1_z2_stats.fb_armed++;
        fb_armed_frame = sof_frame;
    }
}

// Alt 0 only stops re-arming. Any OUT transfer already armed stays queued for the next stream.
static void close_stream_eps(uint8_t rhport) {
    (void)rhport;
    uac1.ep_data_open = false;
    uac1.ep_fb_open = false;
}

// Both alts use the same data OUT (582 B) and feedback IN (3 B) endpoints, so they are activated once
// per bus reset and left enabled across alt changes. Re-activating would make TinyUSB disable the enabled
// OUT EP, and in polled slave mode that waits forever for a global OUT NAK nobody pops from the RX FIFO.
static bool open_stream_eps(uint8_t rhport, uint8_t alt) {
    if (!uac1.eps_activated) {
        TU_ASSERT(usbd_edpt_iso_activate(rhport, (tusb_desc_endpoint_t const *)usb_audio_data_ep_desc[alt - 1]));
        TU_ASSERT(usbd_edpt_iso_activate(rhport, (tusb_desc_endpoint_t const *)usb_audio_fb_ep_desc[alt - 1]));
        uac1.eps_activated = true;
    }
    uac1.ep_data_open = true;
    uac1.ep_fb_open = true;
    if (!usbd_edpt_busy(rhport, AUDIO_OUT_ENDPOINT)) {
        arm_data_out(rhport);
    }
    if (!usbd_edpt_busy(rhport, AUDIO_IN_ENDPOINT)) {
        arm_feedback(rhport);
    }
    return true;
}

// Alts: 0 = zero bandwidth, 1 = stereo 16-bit, 2 = stereo 24-bit.
static bool apply_alt(uint8_t rhport, uint8_t alt) {
    if (alt > 2) {
        return false;
    }
    uac1_z2_stats.set_alt_count++;
    if (alt == uac1.cur_alt) {
        return true;
    }
    uac1.cur_alt = alt;
    close_stream_eps(rhport);
    return alt == 0 || open_stream_eps(rhport, alt);
}

static bool handle_fu_get(uint8_t rhport, tusb_control_request_t const *req) {
    static uint8_t m;
    static int16_t v;
    uint8_t cs = TU_U16_HIGH(req->wValue);
    if (cs == 1 && req->bRequest == 0x81) {            // MUTE GET_CUR
        m = mute;
        return tud_control_xfer(rhport, req, &m, 1);
    }
    if (cs != 2) {                                      // VOLUME
        return false;
    }
    switch (req->bRequest) {
        case 0x81: v = volume; break;                   // GET_CUR
        case 0x82: v = MIN_VOLUME; break;               // GET_MIN
        case 0x83: v = MAX_VOLUME; break;               // GET_MAX
        case 0x84: v = VOLUME_RESOLUTION; break;        // GET_RES
        default: return false;
    }
    return tud_control_xfer(rhport, req, &v, 2);
}

static bool uac1_driver_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    if (stage == CONTROL_STAGE_SETUP) {
        if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
            uint8_t itf = TU_U16_LOW(req->wIndex);
            if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
                uint8_t alt = TU_U16_LOW(req->wValue);
                if (itf == uac1.as_itf) {
                    TU_VERIFY(apply_alt(rhport, alt));
                } else if (alt != 0 || (itf != uac1.ac_itf && itf != uac1.vendor_itf)) {
                    return false;
                }
                return tud_control_status(rhport, req);
            }
            if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
                static uint8_t alt_resp;
                if (itf == uac1.as_itf) {
                    alt_resp = uac1.cur_alt;
                } else if (itf == uac1.ac_itf || itf == uac1.vendor_itf) {
                    alt_resp = 0;
                } else {
                    return false;
                }
                return tud_control_xfer(rhport, req, &alt_resp, 1);
            }
            return false;
        }

        if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
            return false;
        }
        uac1_z2_stats.class_requests++;
        uint8_t recipient = req->bmRequestType_bit.recipient;
        bool is_get = req->bmRequestType_bit.direction == TUSB_DIR_IN;

        if (recipient == TUSB_REQ_RCPT_INTERFACE) {
            if (TU_U16_LOW(req->wIndex) != uac1.ac_itf || TU_U16_HIGH(req->wIndex) != UAC1_FEATURE_UNIT_ID) {
                return false;
            }
            if (is_get) {
                return handle_fu_get(rhport, req);
            }
        } else if (recipient == TUSB_REQ_RCPT_ENDPOINT) {
            if (TU_U16_LOW(req->wIndex) != AUDIO_OUT_ENDPOINT) {
                return false;
            }
            if (is_get) {
                static uint8_t freq_bytes[3];
                if (req->bRequest != UAC1_REQ_GET_CUR || TU_U16_HIGH(req->wValue) != UAC1_EP_CTRL_SAMPLING_FREQ) {
                    return false;
                }
                freq_bytes[0] = (uint8_t)selected_rate;
                freq_bytes[1] = (uint8_t)(selected_rate >> 8);
                freq_bytes[2] = (uint8_t)(selected_rate >> 16);
                return tud_control_xfer(rhport, req, freq_bytes, 3);
            }
        } else {
            return false;
        }

        // SET_CUR: take the data stage, apply it below.
        if (req->bRequest != UAC1_REQ_SET_CUR || req->wLength == 0 || req->wLength > sizeof(uac1_ctrl_buf)) {
            return false;
        }
        uac1.pending_cs = TU_U16_HIGH(req->wValue);
        uac1.pending_recipient = recipient;
        return tud_control_xfer(rhport, req, uac1_ctrl_buf, req->wLength);
    }

    if (stage == CONTROL_STAGE_DATA && req->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) {
        uint8_t recipient = uac1.pending_recipient;
        uac1.pending_recipient = 0;
        if (recipient == TUSB_REQ_RCPT_INTERFACE) {
            if (uac1.pending_cs == UAC1_FU_CTRL_MUTE) {
                mute = uac1_ctrl_buf[0] != 0;
            } else if (uac1.pending_cs == UAC1_FU_CTRL_VOLUME) {
                memcpy(&volume, uac1_ctrl_buf, sizeof(volume));
            }
        } else if (recipient == TUSB_REQ_RCPT_ENDPOINT && uac1.pending_cs == UAC1_EP_CTRL_SAMPLING_FREQ) {
            uint32_t f = uac1_ctrl_buf[0] | ((uint32_t)uac1_ctrl_buf[1] << 8) | ((uint32_t)uac1_ctrl_buf[2] << 16);
            if (f != 44100u && f != 48000u && f != 96000u) {
                return false;   // UAC1 says unsupported values are stalled, not clamped
            }
            selected_rate = f;
            uac1_z2_stats.rate_sets++;
        }
    }
    return true;
}

static bool uac1_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    if (ep_addr == AUDIO_OUT_ENDPOINT) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0) {
            uac1_z2_stats.out_packets++;
            uac1_z2_stats.out_bytes += xferred_bytes;
        }
        if (uac1.ep_data_open) {
            arm_data_out(rhport);
        }
        return true;
    }
    if (ep_addr == AUDIO_IN_ENDPOINT) {
        uac1_z2_stats.fb_sent++;
        return true;    // re-armed from the SOF tick
    }
    return ep_addr == NOTIFY_IN_ENDPOINT;
}

// TinyUSB queues an iso IN packet for the next frame's parity. The host polls feedback every 4 ms, so
// always on one parity, and a packet queued for the other parity would wait forever. One still pending
// after 6 frames is flushed (stall, clear stall), and the next SOF queues it again on the other parity.
#define FB_STALE_FRAMES 6u

static void uac1_driver_sof(uint8_t rhport, uint32_t frame_count) {
    sof_frame = frame_count;
    uac1_z2_stats.sof_count++;
    if (!uac1.ep_fb_open) {
        return;
    }
    if (!usbd_edpt_busy(rhport, AUDIO_IN_ENDPOINT)) {
        arm_feedback(rhport);
    } else if (((frame_count - fb_armed_frame) & 0x7FFu) >= FB_STALE_FRAMES) {
        usbd_edpt_stall(rhport, AUDIO_IN_ENDPOINT);
        usbd_edpt_clear_stall(rhport, AUDIO_IN_ENDPOINT);
        uac1_z2_stats.fb_flushes++;
    }
}

static const usbd_class_driver_t uac1_driver = {
    .name = "DSPi_UAC1_Z2",
    .init = uac1_driver_init,
    .deinit = uac1_driver_deinit,
    .reset = uac1_driver_reset,
    .open = uac1_driver_open,
    .control_xfer_cb = uac1_driver_control_xfer_cb,
    .xfer_cb = uac1_driver_xfer_cb,
    .sof = uac1_driver_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &uac1_driver;
}
