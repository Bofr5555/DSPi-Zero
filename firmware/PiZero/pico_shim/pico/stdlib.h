#pragma once

#include <stdio.h>

#include "pico.h"
#include "pico/time.h"
#include "hardware/gpio.h"

// stdio goes to newlib, whose output is not connected to anything on this platform.
bool stdio_init_all(void);
