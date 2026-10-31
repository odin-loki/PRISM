#pragma once
// CUDA havoc mutation kernel (src/cuda/mutate.cu). C++ linkage: nvcc compiles the kernel
// TU as C++, so no C ABI is needed.
#include <cstddef>
#include <cstdint>

namespace prism::cuda {
// Mutates nseeds seeds of seed_len bytes in place; 0 on success.
int havoc(std::uint8_t* host, int seed_len, int nseeds, std::uint64_t tick);
}  // namespace prism::cuda
