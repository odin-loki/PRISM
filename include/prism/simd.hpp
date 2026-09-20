#pragma once

#include "prism/export.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#  ifdef PRISM_EXPORTS
#    define PRISM_SIMD_API __declspec(dllexport)
#  else
#    define PRISM_SIMD_API
#  endif
#else
#  define PRISM_SIMD_API
#endif

namespace prism {

PRISM_SIMD_API uint64_t coverage_hash(const uint8_t* data, size_t n);
PRISM_SIMD_API void havoc(uint8_t* data, size_t n, uint64_t seed);

}  // namespace prism
