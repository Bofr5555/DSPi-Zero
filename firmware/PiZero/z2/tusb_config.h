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
#define CFG_TUD_VENDOR          0

// DSPi's UAC1 + vendor function is claimed by our own class driver (z2/uac1_z2.c), registered through
// usbd_app_driver_get_cb(), as in firmware/DSPi/usb_audio.c. TinyUSB's UAC2-only audio class stays off.
#define CFG_TUD_DFU_RUNTIME     0
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             0
#define CFG_TUD_BTH             0
