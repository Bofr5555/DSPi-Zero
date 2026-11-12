// memset and memcpy in place of newlib-nano's, which go a byte at a time. The Pico SDK swaps in fast
// versions on the RP2350 too, and DSPi clears its buffers with memset on every block.
// Built with -fno-builtin and -fno-tree-loop-distribute-patterns so these loops never become calls to
// themselves. NEON byte-element loads and stores need no alignment, even with the MMU off.

#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    uint8_t b = (uint8_t)c;
#if defined(__ARM_NEON)
    uint8x16_t v = vdupq_n_u8(b);
    while (n >= 64) {
        vst1q_u8(p, v);
        vst1q_u8(p + 16, v);
        vst1q_u8(p + 32, v);
        vst1q_u8(p + 48, v);
        p += 64;
        n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(p, v);
        p += 16;
        n -= 16;
    }
#else
    while (n && ((uintptr_t)p & 3u)) {
        *p++ = b;
        n--;
    }
    uint32_t w = b * 0x01010101u;
    while (n >= 16) {
        uint32_t *q = (uint32_t *)p;
        q[0] = w;
        q[1] = w;
        q[2] = w;
        q[3] = w;
        p += 16;
        n -= 16;
    }
#endif
    while (n--) {
        *p++ = b;
    }
    return s;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
#if defined(__ARM_NEON)
    while (n >= 64) {
        uint8x16_t a = vld1q_u8(s), b = vld1q_u8(s + 16), c = vld1q_u8(s + 32), e = vld1q_u8(s + 48);
        vst1q_u8(d, a);
        vst1q_u8(d + 16, b);
        vst1q_u8(d + 32, c);
        vst1q_u8(d + 48, e);
        s += 64;
        d += 64;
        n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(d, vld1q_u8(s));
        s += 16;
        d += 16;
        n -= 16;
    }
#else
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        while (n >= 16) {
            uint32_t *q = (uint32_t *)d;
            const uint32_t *r = (const uint32_t *)s;
            q[0] = r[0];
            q[1] = r[1];
            q[2] = r[2];
            q[3] = r[3];
            s += 16;
            d += 16;
            n -= 16;
        }
    }
#endif
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
