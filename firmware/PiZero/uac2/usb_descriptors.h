#pragma once

#include <stdint.h>

// UAC2 stereo speaker with explicit feedback, alts 1 (16-bit) and 2 (24-bit) at 44.1, 48 and 96 kHz,
// plus DSPi's vendor interface with its bulk notification endpoint, for DSPi Console.

#define USB_VENDOR_ID           0x2E8B
#define USB_PRODUCT_ID          0xFEAA
#define USB_BCD_DEVICE          0x0300      // UAC2 Zero; DSPi's UAC1 boards report 0x02xx

#define ITF_NUM_AUDIO_CONTROL   0
#define ITF_NUM_AUDIO_STREAMING 1
#define ITF_NUM_VENDOR          2
#define ITF_NUM_TOTAL           3

#define EP_AUDIO_OUT            0x01
#define EP_AUDIO_FB             0x82
#define EP_NOTIFY_IN            0x83
#define NOTIFY_EP_MAX_PKT_HS    512         // High Speed bulk endpoints must be 512 bytes

#define UAC2_ENTITY_INPUT_TERMINAL  0x01
#define UAC2_ENTITY_FEATURE_UNIT    0x02
#define UAC2_ENTITY_OUTPUT_TERMINAL 0x03
#define UAC2_ENTITY_CLOCK           0x04

// Data goes every microframe (bInterval 1), TinyUSB's own DWC2 path for iso OUT: this controller fixes
// an OUT endpoint's frame parity when it is enabled, so a 1 ms interval would need parity found by trial.
// Largest packet: 96 kHz, 3-byte samples, stereo, plus one frame of jitter.
#define UAC2_DATA_INTERVAL      0x01
#define UAC2_EP_OUT_SIZE        ((96000 / 8000 + 1) * 3 * 2)

#define UAC2_AS_ALT_LEN (TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO_DESC_STD_AS_ISO_FB_EP_LEN)

#define UAC2_DSPI_DESC_LEN (TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN + TUD_AUDIO_DESC_CS_AC_LEN \
    + TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN \
    + TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN + TUD_AUDIO_DESC_STD_AS_INT_LEN + 2 * UAC2_AS_ALT_LEN)

extern char usb_serial_string[17];
