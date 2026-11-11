#pragma once

#if BCM_VERSION == 2837
#define CFG_TUSB_MCU            OPT_MCU_BCM2837
#else
#define CFG_TUSB_MCU            OPT_MCU_BCM2835
#endif
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1
// TinyUSB's Full Speed setting selects a dedicated FS PHY. We keep the UTMI+ HS PHY and force
// Full Speed through it (DCFG.DSPD = 1) after init instead. See PiZero_Z0_bringup.md section 9.
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
#define CFG_TUD_AUDIO           0
#define CFG_TUD_VENDOR          1

#define CFG_TUD_VENDOR_EPSIZE      64
#define CFG_TUD_VENDOR_RX_BUFSIZE  4096
#define CFG_TUD_VENDOR_TX_BUFSIZE  1024
