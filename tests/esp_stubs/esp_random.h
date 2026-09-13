#pragma once
#include <stdint.h>
static inline uint32_t esp_random(void) {
    /* Simple PRNG for host tests — not cryptographically secure */
    static uint32_t state = 0xDEADBEEF;
    state = state * 1103515245 + 12345;
    return (state >> 16) & 0x7FFF;
}
