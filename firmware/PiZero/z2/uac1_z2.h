#pragma once

#include <stdint.h>

// Counters for bench checks. Written from the USB polling path, read anywhere.
typedef struct {
    uint32_t class_requests;
    uint32_t set_alt_count;
    uint32_t rate_sets;
    uint32_t sof_count;
    uint32_t fb_armed;
    uint32_t fb_sent;
    uint32_t out_packets;
    uint32_t out_bytes;
    uint32_t suspends;
    uint32_t resumes;
    uint32_t mounts;
    uint32_t fb_flushes;
} uac1_z2_stats_t;

extern volatile uac1_z2_stats_t uac1_z2_stats;

uint32_t uac1_z2_rate(void);   // sampling rate the host last set (48000 until then)
uint8_t uac1_z2_alt(void);     // current AS alt setting
