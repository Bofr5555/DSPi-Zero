#pragma once

#include "pico.h"

// There is no boot ROM to reboot into. The USB loader plays that role (vendor request 0x40).
void reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask);
void rom_reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask);
