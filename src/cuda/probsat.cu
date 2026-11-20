// GPU ProbSAT walkers (roadmap 3.3). Compiled only with PRISM_CUDA=ON.
//
// This mirrors prism::solver::probsat() in src/prism/solver/cnf.cpp: one CUDA
// thread is one independent walker with its own seed, over one CNF held in
// device memory in CSR form (clause -> literals, literal -> clauses). The
// first walker to reach zero unsatisfied clauses writes its assignment and
// raises a flag the others poll. Like the CPU walker, a result here is only
// ever a candidate counterexample: the host maps it back through Cnf::symbols
// and solve() validates it against the original formula in Z3 before anything
// is reported. A walker that finds nothing proves nothing.
//
// Status (2026-11-20): built with nvcc 13.4 (host gcc) in WSL2 and run on an
// RTX 3090 (sm_86) by the doctests "solver: GPU ProbSAT*" and by the
// cube-and-conquer GPU mode (cube.cpp), which hands it the whole CNF and the
// CNFs of open cubes. Every CUDA call is checked; a failure is an error
// string in CudaProbsatInfo, never an answer.

#include "prism/solver.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

__device__ inline std::uint64_t splitmix(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

__device__ inline int lit_index(int l) { return 2 * (l < 0 ? -l : l) + (l < 0 ? 1 : 0); }

struct DevCnf {
    int num_vars;
    int num_clauses;
    const int* cl_start;   // num_clauses + 1
    const int* cl_lits;    // literals
    const int* occ_start;  // 2*num_vars + 3
    const int* occ;        // clause ids per literal index
};

// Per-walker scratch lives in global memory, strided by walker id.
__global__ void probsat_kernel(DevCnf f, std::uint64_t seed, std::uint64_t max_flips, double cb,
                               double eps, std::int8_t* assign_all, int* ntrue_all, int* unsat_all,
                               int* where_all, int* found, std::int8_t* result, unsigned nwalkers) {
    const unsigned w = blockIdx.x * blockDim.x + threadIdx.x;
    if (w >= nwalkers) return;
    std::int8_t* a = assign_all + std::size_t(w) * (f.num_vars + 1);
    int* ntrue = ntrue_all + std::size_t(w) * f.num_clauses;
    int* unsat = unsat_all + std::size_t(w) * f.num_clauses;
    int* where = where_all + std::size_t(w) * f.num_clauses;
    std::uint64_t rng = seed ^ (0x9e3779b97f4a7c15ull * (w + 1));
    for (int v = 1; v <= f.num_vars; ++v) a[v] = std::int8_t(splitmix(rng) & 1);
    int nunsat = 0;
    for (int c = 0; c < f.num_clauses; ++c) {
        int t = 0;
        for (int k = f.cl_start[c]; k < f.cl_start[c + 1]; ++k) {
            int l = f.cl_lits[k];
            t += ((l > 0) == (a[l < 0 ? -l : l] == 1)) ? 1 : 0;
        }
        ntrue[c] = t;
        where[c] = -1;
        if (t == 0) { where[c] = nunsat; unsat[nunsat++] = c; }
    }
    double probs[64];
    for (std::uint64_t flip = 0; nunsat > 0 && flip < max_flips; ++flip) {
        if ((flip & 255u) == 0 && atomicAdd(found, 0) != 0) return;
        const int c = unsat[splitmix(rng) % std::uint64_t(nunsat)];
        const int b0 = f.cl_start[c], len = f.cl_start[c + 1] - b0;
        double sum = 0;
        for (int j = 0; j < len && j < 64; ++j) {
            int brk = 0;
            const int li = lit_index(-f.cl_lits[b0 + j]);
            for (int k = f.occ_start[li]; k < f.occ_start[li + 1]; ++k) brk += ntrue[f.occ[k]] == 1;
            probs[j] = pow(eps + double(brk), -cb);
            sum += probs[j];
        }
        double pick = double(splitmix(rng) >> 11) * (1.0 / 9007199254740992.0) * sum;
        int j = 0;
        for (; j + 1 < len && j + 1 < 64; ++j) {
            if (pick < probs[j]) break;
            pick -= probs[j];
        }
        const int lit = f.cl_lits[b0 + j];
        a[lit < 0 ? -lit : lit] = std::int8_t(lit > 0 ? 1 : 0);
        const int lt = lit_index(lit), lf = lit_index(-lit);
        for (int k = f.occ_start[lt]; k < f.occ_start[lt + 1]; ++k) {
            const int d = f.occ[k];
            if (ntrue[d]++ == 0) {
                const int pos = where[d], last = unsat[--nunsat];
                unsat[pos] = last;
                where[last] = pos;
                where[d] = -1;
            }
        }
        for (int k = f.occ_start[lf]; k < f.occ_start[lf + 1]; ++k) {
            const int d = f.occ[k];
            if (--ntrue[d] == 0) { where[d] = nunsat; unsat[nunsat++] = d; }
        }
    }
    if (nunsat == 0 && atomicCAS(found, 0, 1) == 0)
        for (int v = 0; v <= f.num_vars; ++v) result[v] = a[v];
}

// Host re-check, local so this shared library does not reach back into
// prism_core (same rule as prism::solver::assignment_satisfies).
bool satisfies(const prism::solver::Cnf& cnf, const std::vector<std::int8_t>& a) {
    for (const auto& c : cnf.clauses) {
        bool sat = false;
        for (int l : c) {
            const std::size_t v = std::size_t(std::abs(l));
            if (v < a.size() && (l > 0) == (a[v] == 1)) { sat = true; break; }
        }
        if (!sat) return false;
    }
    return true;
}

}  // namespace

namespace prism::solver {

bool cuda_probsat(const Cnf& cnf, std::uint64_t seed, std::uint64_t max_flips_per_walker,
                  unsigned walkers, std::vector<std::int8_t>& assignment) {
    return cuda_probsat_ex(cnf, seed, max_flips_per_walker, walkers, assignment, nullptr);
}

bool cuda_probsat_ex(const Cnf& cnf, std::uint64_t seed, std::uint64_t max_flips_per_walker, unsigned walkers,
                     std::vector<std::int8_t>& assignment, CudaProbsatInfo* info, std::uint64_t mem_cap) {
    CudaProbsatInfo dummy;
    CudaProbsatInfo& inf = info ? *info : dummy;
    const auto fail = [&](std::string why) {
        inf.error = std::move(why);
        return false;
    };
    const int nv = cnf.num_vars, nc = int(cnf.clauses.size());
    std::size_t maxlen = 0;
    std::vector<int> cl_start{0}, cl_lits;
    for (const auto& c : cnf.clauses) {
        if (c.empty()) return fail("empty clause (unsatisfiable CNF)");
        // kernel scratch bound; the CPU walker handles these
        if (c.size() > 64) return fail("clause longer than 64 literals");
        maxlen = std::max(maxlen, c.size());
        cl_lits.insert(cl_lits.end(), c.begin(), c.end());
        cl_start.push_back(int(cl_lits.size()));
    }
    std::vector<std::vector<int>> occv(std::size_t(2 * nv + 2));
    for (int i = 0; i < nc; ++i)
        for (int l : cnf.clauses[std::size_t(i)])
            occv[std::size_t(2 * std::abs(l) + (l < 0 ? 1 : 0))].push_back(i);
    std::vector<int> occ_start{0}, occ;
    for (const auto& o : occv) {
        occ.insert(occ.end(), o.begin(), o.end());
        occ_start.push_back(int(occ.size()));
    }
    const double cb = maxlen <= 3 ? 2.38 : maxlen <= 4 ? 3.0 : maxlen <= 5 ? 3.7 : 5.4;
    if (walkers == 0) walkers = 1024;
    // Per-walker scratch: an assignment byte per variable and three ints per
    // clause. Cap the walkers so the scratch fits in mem_cap (default 4 GiB).
    const std::uint64_t cap = mem_cap ? mem_cap : (std::uint64_t(4) << 30);
    const std::uint64_t per = std::uint64_t(nv + 1) + 12ull * std::uint64_t(nc ? nc : 1);
    walkers = unsigned(std::min<std::uint64_t>(walkers, std::max<std::uint64_t>(1, cap / per)));
    inf.walkers = walkers;
    {
        int dev = 0;
        cudaDeviceProp prop{};
        if (cudaGetDevice(&dev) != cudaSuccess || cudaGetDeviceProperties(&prop, dev) != cudaSuccess)
            return fail(std::string("no CUDA device: ") + cudaGetErrorString(cudaGetLastError()));
        inf.device = prop.name;
    }
    std::vector<void*> allocs;
    bool alloc_ok = true;
    const auto dev_alloc = [&](std::size_t bytes) -> void* {
        void* p = nullptr;
        if (cudaMalloc(&p, bytes ? bytes : 1) != cudaSuccess) {
            alloc_ok = false;
            return nullptr;
        }
        allocs.push_back(p);
        return p;
    };
    const auto dev_upload = [&](const std::vector<int>& v) -> const int* {
        void* p = dev_alloc(sizeof(int) * v.size());
        if (p && !v.empty() && cudaMemcpy(p, v.data(), sizeof(int) * v.size(), cudaMemcpyHostToDevice) != cudaSuccess)
            alloc_ok = false;
        return static_cast<const int*>(p);
    };
    const auto free_all = [&] {
        for (void* p : allocs) cudaFree(p);
        allocs.clear();
    };
    DevCnf f{nv, nc, dev_upload(cl_start), dev_upload(cl_lits), dev_upload(occ_start), dev_upload(occ)};
    const std::size_t ncl = std::size_t(nc ? nc : 1);
    auto* assign_all = static_cast<std::int8_t*>(dev_alloc(std::size_t(walkers) * std::size_t(nv + 1)));
    auto* ntrue = static_cast<int*>(dev_alloc(sizeof(int) * std::size_t(walkers) * ncl));
    auto* unsat = static_cast<int*>(dev_alloc(sizeof(int) * std::size_t(walkers) * ncl));
    auto* where = static_cast<int*>(dev_alloc(sizeof(int) * std::size_t(walkers) * ncl));
    auto* found = static_cast<int*>(dev_alloc(sizeof(int)));
    auto* result = static_cast<std::int8_t*>(dev_alloc(std::size_t(nv + 1)));
    if (!alloc_ok || cudaMemset(found, 0, sizeof(int)) != cudaSuccess) {
        const std::string e = cudaGetErrorString(cudaGetLastError());
        free_all();
        return fail("device allocation failed: " + e);
    }
    cudaEvent_t e0{}, e1{};
    cudaEventCreate(&e0);
    cudaEventCreate(&e1);
    const unsigned block = 128, grid = (walkers + block - 1) / block;
    cudaEventRecord(e0);
    probsat_kernel<<<grid, block>>>(f, seed, max_flips_per_walker, cb, 1.0, assign_all, ntrue, unsat,
                                    where, found, result, walkers);
    cudaEventRecord(e1);
    int h_found = 0;
    const cudaError_t launch = cudaGetLastError();
    const cudaError_t sync = cudaDeviceSynchronize();
    bool ok = launch == cudaSuccess && sync == cudaSuccess &&
              cudaMemcpy(&h_found, found, sizeof(int), cudaMemcpyDeviceToHost) == cudaSuccess;
    float ms = 0;
    if (cudaEventElapsedTime(&ms, e0, e1) == cudaSuccess) inf.kernel_s = ms / 1000.0;
    cudaEventDestroy(e0);
    cudaEventDestroy(e1);
    if (!ok) inf.error = std::string("kernel failed: ") + cudaGetErrorString(launch != cudaSuccess ? launch : sync);
    if (ok && h_found) {
        assignment.assign(std::size_t(nv + 1), 0);
        ok = cudaMemcpy(assignment.data(), result, std::size_t(nv + 1), cudaMemcpyDeviceToHost) == cudaSuccess;
    }
    free_all();
    // Host-side re-check: the kernel's claim is not trusted either.
    return ok && h_found && satisfies(cnf, assignment);
}

}  // namespace prism::solver
