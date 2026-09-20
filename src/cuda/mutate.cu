#include "prism/cuda_mutate.h"

#include <cuda_runtime.h>
#include <cstdint>

// AFL++ include/config.h INTERESTING_8 / INTERESTING_16 / INTERESTING_32.
// Same overlays as src/prism/havoc.cpp. Missing nvcc is a CMake WARNING
// (NOTRUN), not a silent disable of host havoc.

__device__ void overlay_le(uint8_t *data, int n, int i, uint32_t raw, int width) {
    for (int k = 0; k < width; ++k) {
        if (i + k >= n) break;
        data[i + k] = (uint8_t)(raw >> (8 * k));
    }
}

__global__ void havoc_kernel(uint8_t *corpus, int seed_len, int nseeds, uint64_t tick) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int nbytes = nseeds * seed_len;
    if (idx >= nbytes) return;
    uint64_t x = (uint64_t)idx * 0x9E3779B97F4A7C15ull ^ tick;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    int seed_i = idx / seed_len;
    int off = idx % seed_len;
    uint8_t *seed = corpus + seed_i * seed_len;
    int kind = (int)(x & 7);
    if (kind == 0) {
        seed[off] ^= (uint8_t)(x >> 8);
    } else if (kind == 1) {
        seed[off] = (uint8_t)(x >> 8);
    } else if (kind == 2) {
        seed[off] = 0xFF;
    } else if (kind == 3) {
        seed[off] = 0x00;
    } else if (kind == 4) {
        const int8_t interesting8[9] = {-128, -1, 0, 1, 16, 32, 64, 100, 127};
        overlay_le(seed, seed_len, off, (uint32_t)(uint8_t)interesting8[(x >> 8) % 9], 1);
    } else if (kind == 5) {
        const int16_t interesting16[10] = {
            -32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767};
        overlay_le(seed, seed_len, off, (uint32_t)(uint16_t)interesting16[(x >> 8) % 10], 2);
    } else if (kind == 6) {
        const int32_t interesting32[9] = {
            (int32_t)0x80000000, -100663046, -32769, 32768,
            65535, 65536, 100663045, 2139095040, 2147483647};
        overlay_le(seed, seed_len, off, (uint32_t)interesting32[(x >> 8) % 9], 4);
    } else {
        int j = (int)((x >> 16) % (uint64_t)seed_len);
        uint8_t t = seed[off];
        seed[off] = seed[j];
        seed[j] = t;
    }
}

extern "C" int prism_cuda_havoc(uint8_t *host, int seed_len, int nseeds, uint64_t tick) {
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
