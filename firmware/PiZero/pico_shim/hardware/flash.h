#pragma once

#include "pico.h"

// RAM-backed stand-in for the Pico 2's 4 MB QSPI flash. Only the top PICO_SHIM_FLASH_WINDOW_BYTES are
// backed, by a RAM array that XIP_BASE is offset against, so flash offsets match the RP2350 build.
// Erase sets bytes to 0xFF and program only clears bits, as NOR flash does. Contents are lost at reboot.
#define FLASH_PAGE_SIZE   256u
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_BLOCK_SIZE  65536u

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)
#endif
#define PICO_SHIM_FLASH_WINDOW_BYTES (128u * 1024u)

extern uint8_t pico_shim_flash_window[PICO_SHIM_FLASH_WINDOW_BYTES];
#define XIP_BASE ((uintptr_t)pico_shim_flash_window - (PICO_FLASH_SIZE_BYTES - PICO_SHIM_FLASH_WINDOW_BYTES))

// Offsets outside the backed window panic.
void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);
void pico_shim_flash_init(void);    // erases the window, as a blank flash reads
