#pragma once

#include <stdio.h>

#include "pico.h"
#include "pico/time.h"
#include "hardware/gpio.h"

// stdio is newlib's; its output goes to the bring-up UART (_write in pico_shim.c).
bool stdio_init_all(void);
