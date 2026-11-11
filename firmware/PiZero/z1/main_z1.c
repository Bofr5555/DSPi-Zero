// Milestone Z1: TinyUSB vendor-class device at Full Speed that echoes every bulk OUT byte back on IN.
// Pass test: firmware/PiZero/tools/z1_vendor_echo.py. Notes in Documentation/Porting/PiZero_Z0_bringup.md section 9.

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

#define DWC2_BASE     USB_OTG_GLOBAL_BASE
#define DWC2_REG(off) (*(volatile uint32_t *)(DWC2_BASE + (off)))
#define GOTGCTL       0x000
#define GUSBCFG       0x00C
#define GRSTCTL       0x010
#define GINTSTS       0x014
#define GSNPSID       0x040
#define GHWCFG2       0x048
#define GHWCFG4       0x050
#define DCFG          0x800
#define DCTL          0x804
#define DSTS          0x808
#define PCGCCTL       0xE00

// Z1 polls the controller from the main loop and never unmasks IRQs, so TinyUSB's IRQ hooks are no-ops.
void BP_EnableIRQ(IRQn_Type irq) {
    (void)irq;
}

void BP_DisableIRQ(IRQn_Type irq) {
    (void)irq;
}

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static volatile uint32_t mount_events, unmount_events, suspend_events, resume_events;
static uint32_t rx_bytes, tx_bytes;

void tud_mount_cb(void) {
    mount_events++;
}

void tud_umount_cb(void) {
    unmount_events++;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    suspend_events++;
}

void tud_resume_cb(void) {
    resume_events++;
}

// Echo only what the IN FIFO can take, so a slow host never makes us drop bytes.
static void echo_task(void) {
    uint8_t buf[CFG_TUD_VENDOR_EPSIZE];
    uint32_t n = tud_vendor_available();
    uint32_t room = tud_vendor_write_available();
    if (n > room) {
        n = room;
    }
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    if (n == 0) {
        return;
    }
    n = tud_vendor_read(buf, n);
    rx_bytes += n;
    tx_bytes += tud_vendor_write(buf, n);
    tud_vendor_write_flush();
}

static void print_kv(const char *label, uint32_t v) {
    uart_puts(label);
    uart_put_dec(v);
}

static void print_reg(const char *label, uint32_t off) {
    uart_puts(label);
    uart_put_hex(DWC2_REG(off));
}

static void dump_regs(const char *when) {
    uart_puts("regs ");
    uart_puts(when);
    print_reg(": GOTGCTL ", GOTGCTL);
    print_reg(" GUSBCFG ", GUSBCFG);
    print_reg(" GRSTCTL ", GRSTCTL);
    print_reg(" GINTSTS ", GINTSTS);
    print_reg(" DCFG ", DCFG);
    print_reg(" DCTL ", DCTL);
    print_reg(" DSTS ", DSTS);
    print_reg(" PCGCCTL ", PCGCCTL);
    uart_puts("\n");
}

int main(void) {
    board_led_init();

    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    boot_check(false);
    uart_puts("\nDSPi PiZero Z1 vendor echo (" __DATE__ " " __TIME__ ")\n");

    uint32_t res[4];
    int before = mbox_get_power_state(VCMAILBOX_DEVICE_USB_HCD);
    mbox_last_result(res);
    print_kv("USB power before ", (uint32_t)before);
    print_kv(" (err ", res[0]);
    uart_puts(")");
    int power = mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);
    mbox_last_result(res);
    print_kv(", set-on reply ", (uint32_t)power);
    print_kv(" (err ", res[0]);
    uart_puts(" code ");
    uart_put_hex(res[1]);
    uart_puts(" tag ");
    uart_put_hex(res[2]);
    uart_puts(" state ");
    uart_put_hex(res[3]);
    print_kv("), after ", (uint32_t)mbox_get_power_state(VCMAILBOX_DEVICE_USB_HCD));
    uart_puts("\n");

    uart_puts("DWC2 GSNPSID ");
    uart_put_hex(DWC2_REG(GSNPSID));
    uart_puts(" GHWCFG2 ");
    uart_put_hex(DWC2_REG(GHWCFG2));
    uart_puts(" GHWCFG4 ");
    uart_put_hex(DWC2_REG(GHWCFG4));
    uart_puts("\n");

    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    bool ok = tusb_init(0, &dev_init);
    uart_puts(ok ? "tusb_init ok\n" : "tusb_init FAILED\n");

    // Force Full Speed on the UTMI+ PHY before the host's first bus reset can start HS chirp.
    tud_disconnect();
    DWC2_REG(DCFG) = (DWC2_REG(DCFG) & ~3u) | 1u;
    board_delay_us(20000);
    tud_connect();
    dump_regs("after connect");

    uint32_t last_toggle = board_micros();
    uint32_t last_beat = last_toggle;
    uint32_t seconds = 0;
    int led_on = 1;
    bool tried_hs = false;

    for (;;) {
        tud_int_handler(0);
        tud_task();
        echo_task();

        uint32_t now = board_micros();
        if (now - last_toggle >= 500000u) {
            last_toggle += 500000u;
            led_on = !led_on;
            board_led_write(led_on);
        }
        if (now - last_beat >= 1000000u) {
            last_beat += 1000000u;
            seconds++;
            print_kv("hb ", seconds);
            print_kv(" s: mounted ", tud_mounted() ? 1u : 0u);
            uart_puts(" speed ");
            tusb_speed_t sp = tud_speed_get();
            uart_puts(sp == TUSB_SPEED_HIGH ? "HS" : sp == TUSB_SPEED_FULL ? "FS" : "LS");
            print_kv(" mount/umount/susp/res ", mount_events);
            print_kv("/", unmount_events);
            print_kv("/", suspend_events);
            print_kv("/", resume_events);
            print_kv(" rx ", rx_bytes);
            print_kv(" tx ", tx_bytes);
            print_reg(" DSTS ", DSTS);
            print_reg(" GINTSTS ", GINTSTS);
            print_reg(" GOTGCTL ", GOTGCTL);
            uart_puts("\n");

            // Diagnostic: if the host has not reset the bus after 8 s, drop the Full Speed override
            // to see whether DSPD=1 is what hides the device.
            if (!tried_hs && seconds == 8 && mount_events == 0 && !(DWC2_REG(GINTSTS) & (1u << 12))) {
                tried_hs = true;
                tud_disconnect();
                DWC2_REG(DCFG) = DWC2_REG(DCFG) & ~3u;
                board_delay_us(20000);
                tud_connect();
                dump_regs("retry with DSPD=0 (High Speed allowed)");
            }
        }
    }
}
