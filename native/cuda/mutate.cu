#include "helix/cuda_mutate.h"

#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>

// One thread flips a byte of a seed according to a per-thread RNG.
__global__ void havoc_kernel(uint8_t *corpus, int seed_len, int nseeds, uint64_t tick) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nseeds * seed_len) return;
    uint64_t x = (uint64_t)idx * 0x9E3779B97F4A7C15ull ^ tick;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    if ((x & 7) == 0) {
        corpus[idx] ^= (uint8_t)(x >> 8);
    }
}

extern "C" int helix_cuda_havoc(uint8_t *host, int seed_len, int nseeds, uint64_t tick) {
    if (seed_len <= 0 || nseeds <= 0) return -1;
    size_t bytes = (size_t)seed_len * (size_t)nseeds;
    uint8_t *dev = nullptr;
    if (cudaMalloc(&dev, bytes) != cudaSuccess) return -2;
    cudaMemcpy(dev, host, bytes, cudaMemcpyHostToDevice);
    int threads = 256;
    int blocks = (int)((bytes + threads - 1) / threads);
    havoc_kernel<<<blocks, threads>>>(dev, seed_len, nseeds, tick);
    cudaMemcpy(host, dev, bytes, cudaMemcpyDeviceToHost);
    cudaFree(dev);
    return 0;
}
