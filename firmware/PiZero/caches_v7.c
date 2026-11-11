// BCM2837 (AArch32) stand-ins for broadcom-peripherals' cache helpers, whose BCM2837 path is AArch64-only.
// The D-cache stays off until the MMU milestone, so the DWC2 glue's cache maintenance has nothing to do.

#include "broadcom/caches.h"

void init_caches(void) {}

void data_clean(volatile void *starting_address, size_t size) {
    (void)starting_address;
    (void)size;
}

void data_clean_and_invalidate(volatile void *starting_address, size_t size) {
    (void)starting_address;
    (void)size;
}

void data_invalidate(volatile void *starting_address, size_t size) {
    (void)starting_address;
    (void)size;
}
