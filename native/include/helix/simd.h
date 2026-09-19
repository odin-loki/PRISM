#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

#ifdef _WIN32
#  ifdef HELIX_NATIVE_EXPORTS
#    define HELIX_API __declspec(dllexport)
#  else
#    define HELIX_API __declspec(dllimport)
#  endif
#else
#  define HELIX_API
#endif

namespace helix {

HELIX_API uint64_t coverage_hash(const uint8_t *data, size_t n);
HELIX_API void havoc(uint8_t *data, size_t n, uint64_t seed);

}  // namespace helix
