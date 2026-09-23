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
// Status: written without a GPU or nvcc on the development machine; it has
// never been compiled or run. The CPU walker is the tested reference.

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

template <class T>
T* upload(const std::vector<T>& v) {
    T* d = nullptr;
    if (cudaMalloc(&d, sizeof(T) * (v.empty() ? 1 : v.size())) != cudaSuccess) return nullptr;
    if (!v.empty()) cudaMemcpy(d, v.data(), sizeof(T) * v.size(), cudaMemcpyHostToDevice);
    return d;
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
    const int nv = cnf.num_vars, nc = int(cnf.clauses.size());
    std::size_t maxlen = 0;
    std::vector<int> cl_start{0}, cl_lits;
    for (const auto& c : cnf.clauses) {
        if (c.empty()) return false;
        if (c.size() > 64) return false;  // kernel scratch bound; the CPU walker handles these
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
    DevCnf f{nv, nc, upload(cl_start), upload(cl_lits), upload(occ_start), upload(occ)};
    std::int8_t *assign_all = nullptr, *result = nullptr;
    int *ntrue = nullptr, *unsat = nullptr, *where = nullptr, *found = nullptr;
    cudaMalloc(&assign_all, std::size_t(walkers) * std::size_t(nv + 1));
    cudaMalloc(&ntrue, sizeof(int) * std::size_t(walkers) * std::size_t(nc ? nc : 1));
    cudaMalloc(&unsat, sizeof(int) * std::size_t(walkers) * std::size_t(nc ? nc : 1));
    cudaMalloc(&where, sizeof(int) * std::size_t(walkers) * std::size_t(nc ? nc : 1));
    cudaMalloc(&found, sizeof(int));
    cudaMalloc(&result, std::size_t(nv + 1));
    cudaMemset(found, 0, sizeof(int));
    const unsigned block = 128, grid = (walkers + block - 1) / block;
    probsat_kernel<<<grid, block>>>(f, seed, max_flips_per_walker, cb, 1.0, assign_all, ntrue, unsat,
                                    where, found, result, walkers);
    int h_found = 0;
    bool ok = cudaDeviceSynchronize() == cudaSuccess &&
              cudaMemcpy(&h_found, found, sizeof(int), cudaMemcpyDeviceToHost) == cudaSuccess;
    if (ok && h_found) {
        assignment.assign(std::size_t(nv + 1), 0);
        cudaMemcpy(assignment.data(), result, std::size_t(nv + 1), cudaMemcpyDeviceToHost);
    }
    for (void* p : {(void*)f.cl_start, (void*)f.cl_lits, (void*)f.occ_start, (void*)f.occ,
                    (void*)assign_all, (void*)ntrue, (void*)unsat, (void*)where, (void*)found,
                    (void*)result})
        cudaFree(p);
    // Host-side re-check: the kernel's claim is not trusted either.
    return ok && h_found && satisfies(cnf, assignment);
}

}  // namespace prism::solver
