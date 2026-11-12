#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int uint;

// The SDK wraps this in a struct in debug builds. A plain count of microseconds since boot is enough here.
typedef uint64_t absolute_time_t;
