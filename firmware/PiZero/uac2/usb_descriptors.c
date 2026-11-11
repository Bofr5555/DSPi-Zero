// DSPi Zero USB descriptors: a UAC2 stereo speaker with explicit feedback for High Speed, plus DSPi's
// vendor interface (class 0xFF, bulk IN notification endpoint) that DSPi Console binds to.

#include <string.h>

#include "tusb.h"
#include "usb_descriptors.h"

char usb_serial_string[17] = "0000000000000000";

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // IAD triplet, so hosts treat the device as composite (audio function + vendor function).
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VENDOR_ID,
    .idProduct = USB_PRODUCT_ID,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_desc;
}

// One streaming alt: 2 channels at the given subslot size, data OUT and feedback every 1 ms.
#define UAC2_AS_ALT(_alt, _subslot, _bits) \
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, _alt, 0x02, 0x00), \
    TUD_AUDIO_DESC_CS_AS_INT(UAC2_ENTITY_INPUT_TERMINAL, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, \
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x02, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00), \
    TUD_AUDIO_DESC_TYPE_I_FORMAT(_subslot, _bits), \
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EP_AUDIO_OUT, \
                                 (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), \
                                 UAC2_EP_OUT_SIZE, UAC2_DATA_INTERVAL), \
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE, \
                                AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001), \
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(EP_AUDIO_FB, 4, 0x04)

#define FU_CTRL_MUTE_VOL ((AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | \
                          (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS))

#define VENDOR_DESC_LEN (9 + 7)
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC2_DSPI_DESC_LEN + VENDOR_DESC_LEN)

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

    TUD_AUDIO_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 0x02, 0x00),
    TUD_AUDIO_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x00),
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,
                         TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN +
                             TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN,
                         AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),
    TUD_AUDIO_DESC_CLK_SRC(UAC2_ENTITY_CLOCK, AUDIO_CLOCK_SOURCE_ATT_INT_PRO_CLK,
                           (AUDIO_CTRL_RW << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), UAC2_ENTITY_INPUT_TERMINAL, 0x00),
    TUD_AUDIO_DESC_INPUT_TERM(UAC2_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, UAC2_ENTITY_CLOCK, 0x02,
                              AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0x0000, 0x00),
    TUD_AUDIO_DESC_OUTPUT_TERM(UAC2_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, UAC2_ENTITY_INPUT_TERMINAL,
                               UAC2_ENTITY_FEATURE_UNIT, UAC2_ENTITY_CLOCK, 0x0000, 0x00),
    // Master mute and volume only, as on DSPi's UAC1 boards.
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(UAC2_ENTITY_FEATURE_UNIT, UAC2_ENTITY_INPUT_TERMINAL, FU_CTRL_MUTE_VOL, 0, 0, 0x00),
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),
    UAC2_AS_ALT(0x01, 2, 16),
    UAC2_AS_ALT(0x02, 3, 24),

    // Vendor interface: DSPi Console's control channel. Commands travel over EP0; EP 0x83 carries
    // device-to-host notifications.
    9, TUSB_DESC_INTERFACE, ITF_NUM_VENDOR, 0x00, 0x01, 0xFF, 0x00, 0x00, 0x00,
    7, TUSB_DESC_ENDPOINT, EP_NOTIFY_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(NOTIFY_EP_MAX_PKT_HS), 0x00,
};

TU_VERIFY_STATIC(sizeof(config_desc) == CONFIG_TOTAL_LEN, "config descriptor length");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

static const char *const string_desc[] = {
    NULL,                       // 0: language
    "GitHub.com/WeebLabs",      // 1: manufacturer, as on DSPi
    "Weeb Labs DSPi Zero",      // 2: product
    usb_serial_string,          // 3: serial, the board's VideoCore serial
};

static uint16_t string_buf[40];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t count;
    if (index == 0) {
        string_buf[1] = 0x0409;
        count = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0])) {
            return NULL;
        }
        const char *s = string_desc[index];
        count = strlen(s);
        if (count > 39) {
            count = 39;
        }
        for (size_t i = 0; i < count; i++) {
            string_buf[1 + i] = (uint8_t)s[i];
        }
    }
    string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return string_buf;
}
