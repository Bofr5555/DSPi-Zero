// Z1 test device: one vendor-class interface with a 64-byte bulk OUT/IN pair for the echo test.
// 0xCAFE is TinyUSB's test VID. This device never ships, so it does not use DSPi's IDs.

#include <string.h>

#include "tusb.h"

#define Z1_VID 0xCAFE
#define Z1_PID 0x5A31

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = Z1_VID,
    .idProduct = Z1_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_desc;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define EP_VENDOR_OUT 0x01
#define EP_VENDOR_IN  0x81

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, EP_VENDOR_OUT, EP_VENDOR_IN, CFG_TUD_VENDOR_EPSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

static const char *const string_desc[] = {
    NULL,                           // 0: language, handled below
    "DSPi",                         // 1: manufacturer
    "DSPi PiZero Z1 vendor echo",   // 2: product
    "Z1-0001",                      // 3: serial
    "Z1 echo",                      // 4: vendor interface
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
