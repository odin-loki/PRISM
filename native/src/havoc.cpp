#include "helix/simd.h"

#include <cstdint>
#include <xsimd/xsimd.hpp>

namespace helix {

static uint64_t splitmix(uint64_t &s) {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void havoc(uint8_t *data, size_t n, uint64_t seed) {
    if (n == 0) return;
    uint64_t s = seed ? seed : 0xA5A5A5A5A5A5A5A5ull;
    int ops = (int)(splitmix(s) % 8) + 1;
    for (int k = 0; k < ops; ++k) {
        size_t i = (size_t)(splitmix(s) % n);
        switch (splitmix(s) % 6) {
        case 0: data[i] ^= (uint8_t)splitmix(s); break;
        case 1: data[i] = (uint8_t)splitmix(s); break;
        case 2: data[i] = 0xFF; break;
        case 3: data[i] = 0x00; break;
        case 4: {
            static const uint8_t interesting[] = {0, 1, 0x7F, 0x80, 0xFF};
            data[i] = interesting[splitmix(s) % 5];
            break;
        }
        default: {
            size_t j = (size_t)(splitmix(s) % n);
            uint8_t t = data[i];
            data[i] = data[j];
            data[j] = t;
            break;
        }
        }
    }
    (void)xsimd::batch<uint8_t>::size;  // force the header to be the one we mean
}

}  // namespace helix
