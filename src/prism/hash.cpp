#include "prism/simd.hpp"

#include <cstddef>
#include <cstdint>
#include <xsimd/xsimd.hpp>

namespace prism {

uint64_t coverage_hash(const uint8_t* data, size_t n) {
    using batch = xsimd::batch<uint64_t>;
    constexpr std::size_t lanes = batch::size;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    const size_t chunk = lanes * sizeof(uint64_t);
    while (i + chunk <= n) {
        alignas(batch) uint64_t tmp[lanes];
        for (std::size_t k = 0; k < lanes; ++k) {
            uint64_t lane = 0;
            const uint8_t* p = data + i + k * 8;
            for (int b = 0; b < 8; ++b) lane |= (uint64_t)p[b] << (8 * b);
            tmp[k] = lane;
        }
        batch v = batch::load_aligned(tmp);
        alignas(batch) uint64_t out[lanes];
        v.store_aligned(out);
        for (std::size_t k = 0; k < lanes; ++k) {
            h ^= out[k] * 0xBF58476D1CE4E5B9ull;
            h = (h << 13) | (h >> 51);
            h *= 0x94D049BB133111EBull;
        }
        i += chunk;
    }
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
