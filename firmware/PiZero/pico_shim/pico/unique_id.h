#pragma once

#include "pico.h"

// The board ID comes from the VideoCore board serial (mbox_board_serial), as in the UAC2 image.
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8

typedef struct {
    uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
} pico_unique_board_id_t;

void pico_get_unique_board_id(pico_unique_board_id_t *id_out);
void pico_get_unique_board_id_string(char *id_out, uint len);
