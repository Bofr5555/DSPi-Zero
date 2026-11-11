#pragma once

#include <stdbool.h>

// Prints where the firmware started us and whether the .loadcheck filler at the end of the
// image survived loading. With verbose, or on any mismatch, also dumps memory.
void boot_check(bool verbose);
