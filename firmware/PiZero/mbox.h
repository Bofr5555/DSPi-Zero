#pragma once

#include <stdint.h>

// VideoCore property mailbox queries. Each is bounded to 100 ms and returns 0 if the
// firmware rejects the tag or the request times out.
uint32_t mbox_clock_rate(uint32_t clock_id);          // the rate the firmware has set
uint32_t mbox_clock_rate_measured(uint32_t clock_id); // the rate the firmware measures now
uint32_t mbox_board_revision(void);
uint32_t mbox_firmware_revision(void);
uint64_t mbox_board_serial(void);    // 0 if unavailable
int mbox_arm_memory(uint32_t *base, uint32_t *size);
// Returns the firmware's reply state (bit 0 on, bit 1 device missing), or -1 on failure.
int mbox_set_power_state(uint32_t device_id, int on);
int mbox_get_power_state(uint32_t device_id);
// After a failed call: {error kind (0 ok, 1 send timeout, 2 reply timeout, 3 firmware error,
// 4 tag not answered), response code word, tag status word, second value word}.
void mbox_last_result(uint32_t out[4]);
