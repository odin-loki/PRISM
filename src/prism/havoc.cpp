#include "prism/simd.hpp"
#ifdef PRISM_HAS_CUDA
#include "prism/cuda_mutate.h"
#endif

#include <cstdint>
#include <limits>
#include <xsimd/xsimd.hpp>

namespace prism {

static uint64_t splitmix(uint64_t& s) {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void overlay_le(uint8_t* data, size_t n, size_t i, uint32_t raw, int width) {
    for (int k = 0; k < width; ++k) {
        if (i + static_cast<size_t>(k) >= n) break;
        data[i + static_cast<size_t>(k)] = static_cast<uint8_t>(raw >> (8 * k));
    }
}

void havoc(uint8_t* data, size_t n, uint64_t seed) {
    if (n == 0) return;
#ifdef PRISM_HAS_CUDA
    // GPU INTERESTING_8/16/32 overlays. cudaMalloc failure is a CPU fallback,
    // never a silent disable of the algorithm and never a proof.
    if (n <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
        prism_cuda_havoc(data, static_cast<int>(n), 1, seed) == 0) {
        return;
    }
#endif
    uint64_t s = seed ? seed : 0xA5A5A5A5A5A5A5A5ull;
    int ops = (int)(splitmix(s) % 8) + 1;
    for (int k = 0; k < ops; ++k) {
        size_t i = (size_t)(splitmix(s) % n);
        switch (splitmix(s) % 8) {
        case 0: data[i] ^= (uint8_t)splitmix(s); break;
        case 1: data[i] = (uint8_t)splitmix(s); break;
        case 2: data[i] = 0xFF; break;
        case 3: data[i] = 0x00; break;
        case 4: {
            // AFL++ INTERESTING_8 (config.h)
            static const int8_t interesting8[] = {-128, -1, 0, 1, 16, 32, 64, 100, 127};
            overlay_le(data, n, i,
                       static_cast<uint32_t>(static_cast<uint8_t>(
                           interesting8[splitmix(s) % 9])),
                       1);
            break;
        }
        case 5: {
            static const int16_t interesting16[] = {
                -32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767};
            overlay_le(data, n, i,
                       static_cast<uint32_t>(static_cast<uint16_t>(
                           interesting16[splitmix(s) % 10])),
                       2);
            break;
        }
        case 6: {
            static const int32_t interesting32[] = {
                std::numeric_limits<int32_t>::min(), -100663046, -32769, 32768,
                65535, 65536, 100663045, 2139095040, 2147483647};
            overlay_le(data, n, i,
                       static_cast<uint32_t>(interesting32[splitmix(s) % 9]),
                       4);
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
    (void)xsimd::batch<uint8_t>::size;
}

}  // namespace prism
