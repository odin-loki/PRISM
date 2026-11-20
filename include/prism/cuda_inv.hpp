#pragma once

// Data-driven loop invariant candidates (docs/PIR.md "Loop proofs by CHC
// (Spacer)", "Data-driven candidates"). For every direction d (K signed
// coefficients over feature indices) the minimum and the maximum of
// sum_k coef[d][k] * samples[s][idx[d][k]] over all samples s.
//
// Exact int64 arithmetic: the caller keeps only features whose sampled values
// satisfy |v| < 2^31, |coef| <= 2 and K <= 4, so no sum can overflow. The GPU
// kernel (src/cuda/invdata.cu) and the CPU loop (src/prism/pir/chc.inc,
// direction_bounds_cpu) compute the same function bit for bit: the GPU only
// makes it faster, it never changes a candidate. Candidates are never
// assumed; Houdini proves or drops each one (Law 4 spirit: a guess is not a
// proof).

#include <cstdint>

namespace prism::cuda {

// idx and coef: ndirs * k entries (row d = direction d; coef 0 = unused slot).
// samples: nsamples * nfeat, row-major. Returns 0 on success; nonzero when no
// device or a CUDA call failed (the caller then uses the CPU loop).
// kernel_ms: device time of the copies and the kernel (cudaEvent), or -1.
int direction_bounds(const std::int64_t* samples, int nsamples, int nfeat, const std::int16_t* idx,
                     const std::int8_t* coef, int ndirs, int k, std::int64_t* out_min, std::int64_t* out_max,
                     double* kernel_ms);

}  // namespace prism::cuda
