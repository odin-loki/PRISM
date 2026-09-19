#include "helix/simd.h"

#include <cstddef>

#ifdef _WIN32
#  define HELIX_CAPI __declspec(dllexport)
#else
#  define HELIX_CAPI
#endif

extern "C" {

HELIX_CAPI unsigned long long helix_coverage_hash(const unsigned char *data, size_t n) {
    return helix::coverage_hash(data, n);
}

HELIX_CAPI void helix_havoc(unsigned char *data, size_t n, unsigned long long seed) {
    helix::havoc(data, n, seed);
}

}
