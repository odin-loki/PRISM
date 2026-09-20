#include "prism/simd.hpp"

#include <cstddef>

#ifdef _WIN32
#  define PRISM_CAPI __declspec(dllexport)
#else
#  define PRISM_CAPI
#endif

extern "C" {

PRISM_CAPI unsigned long long prism_coverage_hash(const unsigned char* data, size_t n) {
    return prism::coverage_hash(data, n);
}

PRISM_CAPI void prism_havoc(unsigned char* data, size_t n, unsigned long long seed) {
    prism::havoc(data, n, seed);
}

}
