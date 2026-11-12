#pragma once

// UAC2 at High Speed: TinyUSB's own audio class driver, interrupt-driven, on the patched DWC2 driver
// (tusb_dwc2/dcd_dwc2.c). See PiZero_Z0_bringup.md section 14.

#include "usb_descriptors.h"

#if BCM_VERSION == 2837
#define CFG_TUSB_MCU            OPT_MCU_BCM2837
#else
#define CFG_TUSB_MCU            OPT_MCU_BCM2835
#endif
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED

// Slave (FIFO) mode. DMA mode would need cache maintenance once the D-cache is on.
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_DWC2_DMA_ENABLE   0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0   // the vendor interface is claimed by a small app driver in main_uac2.c
#define CFG_TUD_AUDIO           1

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN               UAC2_DSPI_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT               1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ            64

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE        96000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX  3
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                 1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX          UAC2_EP_OUT_SIZE
// Room for 8 of the largest 1 ms packets. FIFO-count feedback holds it about half full.
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ       (8 * UAC2_EP_OUT_SIZE)

#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP            1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION 0
