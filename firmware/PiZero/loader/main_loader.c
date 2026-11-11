// USB loader. The SD card holds this as kernel.img, loaded at 0x2000000 (boot/config_loader.txt). It
// enumerates as a vendor device, receives an image into 0x8000 over bulk OUT, checks its CRC and jumps
// to it, so new builds go over USB instead of on the card. Protocol in loader_proto.h.
// The LED gives two short flashes a second while the loader waits.

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "mbox.h"
#include "uart.h"

#include "tusb.h"

#include "broadcom/defines.h"
#include "broadcom/interrupts.h"
#include "broadcom/gen/vcmailbox.h"

#include "loader_proto.h"

#define DWC2_DCFG (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + 0x800))

extern uint32_t boot_info[4];   // firmware r0-r2, passed on to the image unchanged

void BP_EnableIRQ(IRQn_Type irq) {
    (void)irq;
}

void BP_DisableIRQ(IRQn_Type irq) {
    (void)irq;
}

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static struct {
    uint32_t state;
    uint32_t size;
    uint32_t received;
    uint32_t max_size;
} status = {.max_size = LOADER_IMAGE_MAX};

static uint32_t expected_crc;
static bool go_pending;
static bool reboot_pending;

static uint8_t *const image = (uint8_t *)LOADER_IMAGE_BASE;

// CRC-32 as zlib computes it (reflected, polynomial 0xEDB88320), a nibble at a time with no table.
static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 2; k++) {
            static const uint32_t t[16] = {
                0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
            };
            crc = (crc >> 4) ^ t[crc & 15u];
        }
    }
    return ~crc;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    static uint8_t buf[8];
    bool in = req->bmRequestType_bit.direction == TUSB_DIR_IN;

    switch (req->bRequest) {
        case LOADER_REQ_LOAD_BEGIN:
            if (in || req->wLength != 8) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                return tud_control_xfer(rhport, req, buf, 8);
            }
            if (stage == CONTROL_STAGE_DATA) {
                uint32_t size = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
                expected_crc = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
                // Drop anything left in the FIFO from an earlier, abandoned upload.
                uint8_t junk[64];
                while (tud_vendor_available()) {
                    tud_vendor_read(junk, sizeof(junk));
                }
                status.received = 0;
                status.size = size;
                status.state = (size == 0 || size > LOADER_IMAGE_MAX) ? LOADER_STATE_TOO_BIG : LOADER_STATE_RECEIVING;
            }
            return true;

        case LOADER_REQ_STATUS:
            if (!in) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                uint16_t len = req->wLength < sizeof(status) ? req->wLength : (uint16_t)sizeof(status);
                return tud_control_xfer(rhport, req, &status, len);
            }
            return true;

        case LOADER_REQ_GO:
            if (in) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                if (status.state != LOADER_STATE_COMPLETE) {
                    return false;
                }
                if (crc32(image, status.size) != expected_crc) {
                    status.state = LOADER_STATE_CRC_BAD;
                    return false;
                }
                status.state = LOADER_STATE_STARTING;
                return tud_control_status(rhport, req);
            }
            if (stage == CONTROL_STAGE_ACK) {
                go_pending = true;
            }
            return true;

        case LOADER_REQ_REBOOT:
            if (in) {
                return false;
            }
            if (stage == CONTROL_STAGE_SETUP) {
                return tud_control_status(rhport, req);
            }
            if (stage == CONTROL_STAGE_ACK) {
                reboot_pending = true;
            }
            return true;

        default:
            return false;
    }
}

static void receive_task(void) {
    uint32_t n = tud_vendor_available();
    if (n == 0) {
        return;
    }
    if (status.state != LOADER_STATE_RECEIVING) {
        uint8_t junk[64];
        tud_vendor_read(junk, n < sizeof(junk) ? n : sizeof(junk));
        return;
    }
    uint32_t room = status.size - status.received;
    if (n > room) {
        n = room;
    }
    status.received += tud_vendor_read(image + status.received, n);
    if (status.received == status.size) {
        status.state = LOADER_STATE_COMPLETE;
    }
}

// Starts the image at 0x8000 as the GPU firmware would: r0-r2 from the firmware, caches off except the
// I-cache, which is invalidated because the image was just written through the data side.
static void __attribute__((noreturn)) start_image(void) {
    register uint32_t r0 __asm__("r0") = boot_info[0];
    register uint32_t r1 __asm__("r1") = boot_info[1];
    register uint32_t r2 __asm__("r2") = boot_info[2];
    register uint32_t r3 __asm__("r3") = LOADER_LAUNCH_MAGIC;
    register uint32_t entry __asm__("r4") = LOADER_IMAGE_BASE;
    register uint32_t zero __asm__("r5") = 0;
    __asm__ volatile(
#if __ARM_ARCH >= 7
        "dsb\n\t"
        "mcr p15, 0, r5, c7, c5, 0\n\t"     // ICIALLU
        "mcr p15, 0, r5, c7, c5, 6\n\t"     // BPIALL
        "dsb\n\t"
        "isb\n\t"
#else
        "mcr p15, 0, r5, c7, c10, 4\n\t"    // DSB
        "mcr p15, 0, r5, c7, c5, 0\n\t"     // invalidate I-cache
        "mcr p15, 0, r5, c7, c5, 6\n\t"     // flush branch target cache
        "mcr p15, 0, r5, c7, c5, 4\n\t"     // ISB
#endif
        "bx r4\n\t"
        :
        : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(entry), "r"(zero)
        : "memory");
    __builtin_unreachable();
}

int main(void) {
    board_led_init();
    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    uart_puts("\nDSPi PiZero USB loader (" __DATE__ " " __TIME__ ")\n");
    mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);

    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    tusb_init(0, &dev_init);
    // Full Speed on the UTMI+ PHY, as in Z1 (bring-up doc section 9).
    tud_disconnect();
    DWC2_DCFG = (DWC2_DCFG & ~3u) | 1u;
    board_delay_us(20000);
    tud_connect();
    uart_set_buffered(1);

    uint32_t t0 = board_micros();
    for (;;) {
        tud_int_handler(0);
        tud_task();
        receive_task();
        uart_poll();

        if (go_pending || reboot_pending) {
            // Let the status stage finish on the bus, then detach so the host sees the device leave.
            uint32_t start = board_micros();
            while (board_micros() - start < 20000u) {
                tud_int_handler(0);
                tud_task();
            }
            tud_disconnect();
            board_delay_us(100000);
            board_led_write(0);
            if (reboot_pending) {
                board_reboot();
            }
            uart_puts("starting image\n");
            uart_set_buffered(0);
            start_image();
        }

        // Two short flashes a second: the loader is waiting.
        uint32_t phase = (board_micros() - t0) % 1000000u;
        board_led_write(phase < 80000u || (phase >= 200000u && phase < 280000u));
    }
}
