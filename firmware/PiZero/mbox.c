#include "mbox.h"

#include <stdbool.h>

#include "board.h"
#include "broadcom/caches.h"
#include "broadcom/cpu.h"
#include "broadcom/defines.h"
#include "broadcom/gen/vcmailbox.h"

// Own request path instead of broadcom-peripherals' vcmailbox_request(), which polls the wrong
// status register, has no time bound and omits the L2 bus alias. See PiZero_Z0_bringup.md section 8.
#define MBOX_FULL        0x80000000u
#define MBOX_EMPTY       0x40000000u
#define MBOX_CH_PROP     8u
#define MBOX_TIMEOUT_US  100000u
// The bus alias tells the GPU how the ARM sees the buffer. The BCM2835 ARM goes through the VideoCore
// L2 (0x4 alias). The BCM2837 ARM does not, so the GPU must read it uncached (0xC alias).
#if BCM_VERSION == 2835
#define BUS_ALIAS_L2     0x40000000u
#else
#define BUS_ALIAS_L2     0xC0000000u
#endif

// The VideoCore only sees the upper 28 bits of the address, so the buffer must be 16-byte aligned.
static volatile uint32_t mbox_buf[16] __attribute__((aligned(16)));

// Why the last request failed, for diagnostics: 0 ok, 1 send timeout, 2 reply timeout,
// 3 firmware error code, 4 tag not answered.
static uint32_t last_error;

static bool mbox_call(uint32_t timeout_us) {
    uint32_t msg = ((uint32_t)mbox_buf | BUS_ALIAS_L2) | MBOX_CH_PROP;
    uint32_t start = board_micros();

    while (VCMAILBOX->STATUS1 & MBOX_FULL) {
        if (board_micros() - start > timeout_us) {
            last_error = 1;
            return false;
        }
    }
    while (!(VCMAILBOX->STATUS0 & MBOX_EMPTY)) {
        (void)VCMAILBOX->READ;
    }
    COMPLETE_MEMORY_READS;
    data_clean(mbox_buf, sizeof(mbox_buf));     // the GPU reads it from SDRAM, not the ARM's cache
    VCMAILBOX->WRITE = msg;

    for (;;) {
        if (board_micros() - start > timeout_us) {
            last_error = 2;
            return false;
        }
        if (VCMAILBOX->STATUS0 & MBOX_EMPTY) {
            continue;
        }
        if (VCMAILBOX->READ == msg) {
            COMPLETE_MEMORY_READS;
            data_invalidate(mbox_buf, sizeof(mbox_buf));
            if (mbox_buf[1] != VCMAILBOX_CODE_REQUEST_SUCCESSFUL) {
                last_error = 3;
                return false;
            }
            return true;
        }
    }
}

// One tag per request. Returns a pointer to the tag's value words, or 0 on failure.
static volatile uint32_t *mbox_query_t(uint32_t tag, uint32_t value_bytes, uint32_t arg0, uint32_t arg1,
                                       uint32_t timeout_us) {
    uint32_t words = value_bytes / 4u;
    mbox_buf[0] = (6u + words) * 4u;    // header(2) + tag header(3) + values + end tag(1)
    mbox_buf[1] = 0;
    mbox_buf[2] = tag;
    mbox_buf[3] = value_bytes;
    mbox_buf[4] = 0;
    for (uint32_t i = 0; i < words; i++) {
        mbox_buf[5 + i] = 0;
    }
    mbox_buf[5] = arg0;
    if (words > 1) {
        mbox_buf[6] = arg1;
    }
    mbox_buf[5 + words] = 0;

    last_error = 0;
    if (!mbox_call(timeout_us)) {
        return 0;
    }
    // Bit 31 of the tag's status word marks a response the firmware actually filled in.
    if (!(mbox_buf[4] & 0x80000000u)) {
        last_error = 4;
        return 0;
    }
    return &mbox_buf[5];
}

static volatile uint32_t *mbox_query(uint32_t tag, uint32_t value_bytes, uint32_t arg0, uint32_t arg1) {
    return mbox_query_t(tag, value_bytes, arg0, arg1, MBOX_TIMEOUT_US);
}

void mbox_last_result(uint32_t out[4]) {
    out[0] = last_error;
    out[1] = mbox_buf[1];
    out[2] = mbox_buf[4];
    out[3] = mbox_buf[6];
}

uint32_t mbox_clock_rate(uint32_t clock_id) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_CLOCK_RATE, 8, clock_id, 0);
    return v ? v[1] : 0;
}

uint32_t mbox_clock_rate_measured(uint32_t clock_id) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_CLOCK_RATE_MEASURED, 8, clock_id, 0);
    return v ? v[1] : 0;
}

uint32_t mbox_board_revision(void) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_BOARD_REVISION, 4, 0, 0);
    return v ? v[0] : 0;
}

uint32_t mbox_voltage_uv(uint32_t voltage_id) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_VOLTAGE, 8, voltage_id, 0);
    return v ? v[1] : 0;
}

uint32_t mbox_temperature_mc(void) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_TEMPERATURE, 8, 0, 0);
    return v ? v[1] : 0;
}

uint64_t mbox_board_serial(void) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_BOARD_SERIAL, 8, 0, 0);
    return v ? ((uint64_t)v[1] << 32) | v[0] : 0;
}

uint32_t mbox_firmware_revision(void) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_FIRMWARE_REVISION, 4, 0, 0);
    return v ? v[0] : 0;
}

int mbox_arm_memory(uint32_t *base, uint32_t *size) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_ARM_MEMORY, 8, 0, 0);
    if (!v) {
        return 0;
    }
    *base = v[0];
    *size = v[1];
    return 1;
}

int mbox_set_power_state(uint32_t device_id, int on) {
    // State bit 0 is on/off. Bit 1 makes the firmware wait until the power is stable before answering.
    // Powering up with the wait bit can take longer than an ordinary query, so allow a full second.
    volatile uint32_t *v = mbox_query_t(VCMAILBOX_TAG_SET_POWER_STATE, 8, device_id, (on ? 1u : 0u) | 2u, 1000000u);
    if (!v) {
        return -1;
    }
    return (int)(v[1] & 3u);
}

int mbox_get_power_state(uint32_t device_id) {
    volatile uint32_t *v = mbox_query(VCMAILBOX_TAG_GET_POWER_STATE, 8, device_id, 0);
    if (!v) {
        return -1;
    }
    return (int)(v[1] & 3u);
}
