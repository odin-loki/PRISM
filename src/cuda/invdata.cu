#include "prism/cuda_inv.hpp"

#include <cuda_runtime.h>
#include <cstdint>

// Direction bounds for data-driven loop invariant candidates (see
// include/prism/cuda_inv.hpp). One thread per direction; the samples are
// streamed through shared memory in tiles. Same arithmetic as the CPU loop
// direction_bounds_cpu (src/prism/pir/chc.inc): exact int64 sums.

namespace {

constexpr int kThreads = 256;
constexpr int kTile = 64;      // samples per tile
constexpr int kMaxFeat = 32;   // features per sample (the caller keeps <= 32)
constexpr int kMaxK = 4;

__global__ void bounds_kernel(const std::int64_t* samples, int ns, int nf, const std::int16_t* idx,
                              const std::int8_t* coef, int nd, int k, std::int64_t* mn, std::int64_t* mx) {
    __shared__ std::int64_t tile[kTile * kMaxFeat];
    const int d = blockIdx.x * blockDim.x + threadIdx.x;
    int ix[kMaxK];
    int cf[kMaxK];
    for (int j = 0; j < kMaxK; ++j) {
        ix[j] = 0;
        cf[j] = 0;
    }
    if (d < nd)
        for (int j = 0; j < k; ++j) {
            ix[j] = idx[d * k + j];
            cf[j] = coef[d * k + j];
        }
    std::int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int base = 0; base < ns; base += kTile) {
        const int n = (ns - base) < kTile ? (ns - base) : kTile;
        for (int t = threadIdx.x; t < n * nf; t += blockDim.x) tile[t] = samples[static_cast<long long>(base) * nf + t];
        __syncthreads();
        if (d < nd)
            for (int s = 0; s < n; ++s) {
                std::int64_t v = 0;
                for (int j = 0; j < k; ++j) v += static_cast<std::int64_t>(cf[j]) * tile[s * nf + ix[j]];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        __syncthreads();
    }
    if (d < nd) {
        mn[d] = lo;
        mx[d] = hi;
    }
}

}  // namespace

namespace prism::cuda {

int direction_bounds(const std::int64_t* samples, int nsamples, int nfeat, const std::int16_t* idx,
                     const std::int8_t* coef, int ndirs, int k, std::int64_t* out_min, std::int64_t* out_max,
                     double* kernel_ms) {
    if (kernel_ms) *kernel_ms = -1;
    if (nsamples <= 0 || ndirs <= 0 || nfeat <= 0 || nfeat > kMaxFeat || k <= 0 || k > kMaxK) return 1;
    int dev = 0;
    if (cudaGetDeviceCount(&dev) != cudaSuccess || dev <= 0) return 2;
    std::int64_t *ds = nullptr, *dmn = nullptr, *dmx = nullptr;
    std::int16_t* di = nullptr;
    std::int8_t* dc = nullptr;
    cudaEvent_t e0{}, e1{};
    int rc = 0;
    const size_t sb = static_cast<size_t>(nsamples) * nfeat * sizeof(std::int64_t);
    const size_t db = static_cast<size_t>(ndirs) * k;
    auto fail = [&](int r) {
        rc = r;
        return r;
    };
    do {
        if (cudaEventCreate(&e0) != cudaSuccess || cudaEventCreate(&e1) != cudaSuccess) { fail(3); break; }
        if (cudaMalloc(&ds, sb) != cudaSuccess || cudaMalloc(&di, db * sizeof(std::int16_t)) != cudaSuccess ||
            cudaMalloc(&dc, db) != cudaSuccess || cudaMalloc(&dmn, ndirs * sizeof(std::int64_t)) != cudaSuccess ||
            cudaMalloc(&dmx, ndirs * sizeof(std::int64_t)) != cudaSuccess) { fail(4); break; }
        cudaEventRecord(e0);
        if (cudaMemcpy(ds, samples, sb, cudaMemcpyHostToDevice) != cudaSuccess ||
            cudaMemcpy(di, idx, db * sizeof(std::int16_t), cudaMemcpyHostToDevice) != cudaSuccess ||
            cudaMemcpy(dc, coef, db, cudaMemcpyHostToDevice) != cudaSuccess) { fail(5); break; }
        const int blocks = (ndirs + kThreads - 1) / kThreads;
        bounds_kernel<<<blocks, kThreads>>>(ds, nsamples, nfeat, di, dc, ndirs, k, dmn, dmx);
        if (cudaGetLastError() != cudaSuccess) { fail(6); break; }
        if (cudaMemcpy(out_min, dmn, ndirs * sizeof(std::int64_t), cudaMemcpyDeviceToHost) != cudaSuccess ||
            cudaMemcpy(out_max, dmx, ndirs * sizeof(std::int64_t), cudaMemcpyDeviceToHost) != cudaSuccess) { fail(7); break; }
        cudaEventRecord(e1);
        cudaEventSynchronize(e1);
        float ms = 0;
        cudaEventElapsedTime(&ms, e0, e1);
        if (kernel_ms) *kernel_ms = ms;
    } while (false);
    cudaFree(ds);
    cudaFree(di);
    cudaFree(dc);
    cudaFree(dmn);
    cudaFree(dmx);
    if (e0) cudaEventDestroy(e0);
    if (e1) cudaEventDestroy(e1);
    return rc;
}

}  // namespace prism::cuda
