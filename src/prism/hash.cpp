#include "prism/simd.hpp"

#include <cstddef>
#include <cstdint>

namespace prism {

uint64_t coverage_hash(const uint8_t* data, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t lane = 0;
        for (int b = 0; b < 8; ++b) lane |= (uint64_t)data[i + b] << (8 * b);
        h ^= lane * 0xBF58476D1CE4E5B9ull;
        h = (h << 13) | (h >> 51);
        h *= 0x94D049BB133111EBull;
        i += 8;
    }
    uint64_t tail = 0;
    for (size_t b = 0; i + b < n; ++b) tail |= (uint64_t)data[i + b] << (8 * b);
    h ^= tail;
    h ^= (uint64_t)n;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

}  // namespace prism
