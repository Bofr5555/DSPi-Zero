// Milestones Z2 and Z3: enumerate as the DSPi UAC1 device (DSPi's own descriptors, stereo alts only) and
// count the isochronous audio the host sends. Counters are read over EP0 with vendor request 0x4F, so no
// UART is needed (tools/z3_stream_check.py). The LED blinks at 1 Hz idle and 4 Hz while streaming.

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "bootcheck.h"
#include "mbox.h"
#include "uart.h"

#include "tusb.h"

#include "broadcom/defines.h"
#include "broadcom/interrupts.h"
#include "broadcom/gen/vcmailbox.h"

#include "loader_proto.h"
#include "uac1_z2.h"
#include "usb_descriptors.h"

#define DWC2_DCFG (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + 0x800))

// Polled from the main loop with CPU interrupts masked, so TinyUSB's IRQ hooks are no-ops.
void BP_EnableIRQ(IRQn_Type irq) {
    (void)irq;
}

void BP_DisableIRQ(IRQn_Type irq) {
    (void)irq;
}

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

void tud_mount_cb(void) {
    uac1_z2_stats.mounts++;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    uac1_z2_stats.suspends++;
}

void tud_resume_cb(void) {
    uac1_z2_stats.resumes++;
}

static volatile bool reboot_requested;

// Bench-only vendor request, device recipient. 0x4F is unused by DSPi's command set (config.h).
#define REQ_Z_GET_STATS 0x4F
#define Z_STATS_MAGIC   0x5A33u     // "Z3"
#define Z_STATS_VERSION 1u

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint32_t uptime_ms;
    uint32_t rate;
    uint8_t  alt;
    uint8_t  mounted;
    uint16_t reserved;
    uac1_z2_stats_t stats;
} z_stats_reply_t;

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    static z_stats_reply_t reply;
    // Back to the USB loader: acknowledge, then reset once the status stage is done.
    if (req->bRequest == APP_REQ_REBOOT_TO_LOADER && req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, req);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reboot_requested = true;
        }
        return true;
    }
    if (req->bRequest != REQ_Z_GET_STATS || req->bmRequestType_bit.direction != TUSB_DIR_IN) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    reply.magic = Z_STATS_MAGIC;
    reply.version = Z_STATS_VERSION;
    reply.uptime_ms = board_micros() / 1000u;
    reply.rate = uac1_z2_rate();
    reply.alt = uac1_z2_alt();
    reply.mounted = tud_mounted() ? 1u : 0u;
    reply.stats = uac1_z2_stats;
    uint16_t len = req->wLength < sizeof(reply) ? req->wLength : (uint16_t)sizeof(reply);
    return tud_control_xfer(rhport, req, &reply, len);
}

// The serial string becomes the board's 64-bit VideoCore serial in hex, so units stay distinct.
static void set_serial_string(void) {
    uint64_t serial = mbox_board_serial();
    if (serial == 0) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t nib = (uint32_t)(serial >> (60 - 4 * i)) & 0xFu;
        usb_descriptor_str_serial[i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    usb_descriptor_str_serial[16] = '\0';
}

static void print_kv(const char *label, uint32_t v) {
    uart_puts(label);
    uart_put_dec(v);
}

int main(void) {
    board_led_init();
    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    boot_check(false);
    uart_puts("\nDSPi PiZero Z2 UAC1 (" __DATE__ " " __TIME__ ")\n");

    set_serial_string();
    uart_puts("serial ");
    uart_puts(usb_descriptor_str_serial);
    print_kv(", USB power reply ", (uint32_t)mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1));
    uart_puts("\n");

    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    uart_puts(tusb_init(0, &dev_init) ? "tusb_init ok\n" : "tusb_init FAILED\n");

    // Full Speed on the UTMI+ PHY, set before the host's first bus reset (see bring-up doc section 9).
    tud_disconnect();
    DWC2_DCFG = (DWC2_DCFG & ~3u) | 1u;
    board_delay_us(20000);
    tud_connect();

    // From here on a print must never hold up USB polling, which loses frames after 1 ms.
    uart_set_buffered(1);

    uint32_t last_toggle = board_micros();
    uint32_t last_beat = last_toggle;
    uint32_t seconds = 0;
    int led_on = 1;

    for (;;) {
        tud_int_handler(0);
        tud_task();
        uart_poll();
        if (reboot_requested) {
            tud_disconnect();
            board_delay_us(50000);
            board_reboot();
        }

        uint32_t now = board_micros();
        uint32_t half_period = uac1_z2_alt() ? 125000u : 500000u;
        if (now - last_toggle >= half_period) {
            last_toggle = now;
            led_on = !led_on;
            board_led_write(led_on);
        }
        if (now - last_beat >= 1000000u) {
            last_beat += 1000000u;
            seconds++;
            print_kv("hb ", seconds);
            print_kv(" s: mounted ", tud_mounted() ? 1u : 0u);
            print_kv(" alt ", uac1_z2_alt());
            print_kv(" rate ", uac1_z2_rate());
            print_kv(" class req ", uac1_z2_stats.class_requests);
            print_kv(" sof ", uac1_z2_stats.sof_count);
            print_kv(" fb ", uac1_z2_stats.fb_sent);
            print_kv(" out pkts ", uac1_z2_stats.out_packets);
            print_kv(" bytes ", uac1_z2_stats.out_bytes);
            uart_puts("\n");
        }
    }
}
