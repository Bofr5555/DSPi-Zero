#pragma once

// Globals that DSPi's main.c defines. main.c is not part of this image, so z4_globals.c defines them
// instead, with the same types and initial values. Most are declared in DSPi's headers (config.h,
// usb_audio.h); the rest are declared here.

#include <stdint.h>

#include "usb_feedback_controller.h"

// Defined in usb_audio.c, declared only locally by its users in DSPi.
extern volatile uint64_t total_samples_produced;
extern volatile uint64_t start_time_us;

extern usb_feedback_ctrl_t fb_ctrl;

// DSPi's usb_descriptors.c, whose header cannot sit beside uac2's usb_descriptors.h.
extern char usb_descriptor_str_serial[17];
