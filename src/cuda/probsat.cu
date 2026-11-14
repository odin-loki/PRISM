// GPU ProbSAT walkers (roadmap 3.3, docs/GPU_PLAN.md G7).
//
// Built to PTX by clang alone (no CUDA SDK, no nvcc):
//   clang -x cuda --cuda-device-only -nocudainc -nocudalib -Wno-unknown-cuda-version
//         --cuda-gpu-arch=sm_86 -O3 -S
// and embedded in PRISM (CMakeLists.txt, prism/probsat_ptx.inc). The host side
// is src/prism/solver/gpu_sls.cpp on the shared driver loader
// (src/prism/cuda_driver.cpp): CUDA driver API through FORGE dl, device memory
// charged to the FORGE governor pool "cuda:0".
//
// One thread is one independent ProbSAT walker (Balint & Schoening 2012,
// polynomial break-only distribution, as prism::solver::probsat() in
// src/prism/solver/cnf.cpp). Differences from the first version of this file
// (docs/feedback/GPU-FINDINGS.md A.2):
//   * per-walker state is 9 bytes per clause (u8 ntrue, u32 where, u32 unsat)
//     plus one bit per variable instead of 12 bytes per clause plus a byte per
//     variable, and the host sizes the walker count to the memory budget
//     (1024 walkers x 1M clauses no longer asks for 12 GB);
//   * no clause-length cap of 64: the literal to flip is drawn by one-pass
//     weighted reservoir sampling, no per-clause scratch array (the host still
//     rejects clauses longer than 255 literals, the range of the u8 counter);
//   * initialisation is a (walker, clause)-parallel kernel with contiguous
//     writes, not one thread walking every clause;
//   * the walk runs in short launches with all state in device memory, so no
//     launch trips the display driver's watchdog and the host can stop it.
// Like the CPU walker, a result is only ever a candidate: the host re-checks
// every clause, and the portfolio validates the model against the original
// formula in Z3 before anything is reported. A walker that finds nothing
// proves nothing.

#include "probsat_launch.h"

#define PRISM_KERNEL extern "C" __attribute__((global))
#define PS_DEV __attribute__((device)) static inline

PS_DEV ps_u64 gid() {
    return (ps_u64)__nvvm_read_ptx_sreg_ctaid_x() * __nvvm_read_ptx_sreg_ntid_x() + __nvvm_read_ptx_sreg_tid_x();
}
PS_DEV ps_u64 gstride() { return (ps_u64)__nvvm_read_ptx_sreg_nctaid_x() * __nvvm_read_ptx_sreg_ntid_x(); }

PS_DEV ps_u64 splitmix(ps_u64& s) {
    ps_u64 z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
PS_DEV ps_u64 mix3(ps_u64 a, ps_u64 b, ps_u64 c) {
    ps_u64 s = a ^ (0x9e3779b97f4a7c15ull * (b + 1)) ^ (0xc2b2ae3d27d4eb4full * (c + 1));
    return splitmix(s);
}
PS_DEV ps_u32 lit_index(ps_i32 l) { return l < 0 ? 2u * (ps_u32)(-l) + 1u : 2u * (ps_u32)l; }
PS_DEV ps_u32 var_of(ps_i32 l) { return l < 0 ? (ps_u32)(-l) : (ps_u32)l; }
PS_DEV bool lit_true(const ps_u32* a, ps_i32 l) {
    const ps_u32 v = var_of(l);
    return ((a[v >> 5] >> (v & 31u)) & 1u) == (l > 0 ? 1u : 0u);
}

// Random assignment and walker headers, over the walkers x words range.
PRISM_KERNEL void prism_probsat_init_assign(PsLaunch L) {
    for (ps_u64 i = L.base + gid(); i < L.base + L.count; i += gstride()) {
        const ps_u64 w = i / L.words, k = i % L.words;
        L.assign[i] = (ps_u32)mix3(L.seed, w, k);
        if (k == 0) {
            L.walker[w].rng = mix3(L.seed ^ 0x5851f42d4c957f2dull, w, 0);
            L.walker[w].nunsat = 0;
            L.walker[w].done = 0;
        }
    }
}

// ntrue per clause and the unsat lists, over the walkers x clauses range.
PRISM_KERNEL void prism_probsat_init_count(PsLaunch L) {
    for (ps_u64 i = L.base + gid(); i < L.base + L.count; i += gstride()) {
        const ps_u64 w = i / L.num_clauses;
        const ps_u32 c = (ps_u32)(i % L.num_clauses);
        const ps_u32* a = L.assign + w * L.words;
        ps_u32 t = 0;
        for (ps_u32 k = L.cl_start[c]; k < L.cl_start[c + 1]; ++k) t += lit_true(a, L.cl_lits[k]) ? 1u : 0u;
        L.ntrue[i] = (ps_u8)t;
        if (t == 0) {
            const ps_u32 pos = (ps_u32)__nvvm_atom_add_gen_i((int*)&L.walker[w].nunsat, 1);
            L.unsat[w * L.num_clauses + pos] = c;
            L.where[i] = pos;
        }
    }
}

// Up to L.flips flips per walker; state stays in device memory between launches.
PRISM_KERNEL void prism_probsat_walk(PsLaunch L) {
    const ps_u64 w = gid();
    if (w >= L.walkers) return;
    struct PsWalker st = L.walker[w];
    if (st.done) return;
    volatile ps_u32* found = L.found;
    const ps_u64 cb = w * L.num_clauses;
    ps_u32* a = L.assign + w * L.words;
    ps_u8* nt = L.ntrue + cb;
    ps_u32* wh = L.where + cb;
    ps_u32* us = L.unsat + cb;
    const ps_u32* __restrict__ cl_start = L.cl_start;
    const ps_i32* __restrict__ cl_lits = L.cl_lits;
    const ps_u32* __restrict__ occ_start = L.occ_start;
    const ps_u32* __restrict__ occ = L.occ;
    ps_u64 rng = st.rng;
    ps_u32 nun = st.nunsat;
    ps_u32 f = 0;
    for (; f < L.flips && nun != 0; ++f) {
        if ((f & 63u) == 0 && *found != 0) break;
        const ps_u32 c = us[(ps_u32)(((splitmix(rng) >> 32) * (ps_u64)nun) >> 32)];
        const ps_u32 b0 = cl_start[c], b1 = cl_start[c + 1];
        // One-pass weighted reservoir sampling over the clause's literals.
        float sum = 0.0f;
        ps_i32 pick = cl_lits[b0];
        ps_u64 bits = 0;
        ps_u32 nbits = 0;
        for (ps_u32 k = b0; k < b1; ++k) {
            const ps_i32 lit = cl_lits[k];
            // Flipping var(lit) breaks every clause whose only true literal is -lit.
            const ps_u32 li = lit_index(-lit);
            ps_u32 brk = 0;
            for (ps_u32 o = occ_start[li], e = occ_start[li + 1]; o < e && brk < PS_WEIGHTS - 1; ++o)
                brk += nt[occ[o]] == 1 ? 1u : 0u;
            const float wgt = L.weight[brk];
            sum += wgt;
            if (nbits == 0) {
                bits = splitmix(rng);
                nbits = 2;
            }
            const float u = (float)((ps_u32)bits >> 8) * (1.0f / 16777216.0f);
            bits >>= 32;
            --nbits;
            if (u * sum < wgt) pick = lit;
        }
        // pick is false now; flipping its variable makes it true.
        const ps_u32 v = var_of(pick);
        a[v >> 5] ^= 1u << (v & 31u);
        const ps_u32 lt = lit_index(pick), lf = lit_index(-pick);
        for (ps_u32 o = occ_start[lt], e = occ_start[lt + 1]; o < e; ++o) {
            const ps_u32 d = occ[o];
            if (nt[d]++ == 0) {  // d leaves the unsat list
                const ps_u32 pos = wh[d], last = us[--nun];
                us[pos] = last;
                wh[last] = pos;
            }
        }
        for (ps_u32 o = occ_start[lf], e = occ_start[lf + 1]; o < e; ++o) {
            const ps_u32 d = occ[o];
            if (--nt[d] == 0) {  // d joins it
                wh[d] = nun;
                us[nun++] = d;
            }
        }
    }
    if (nun == 0) {
        st.done = 1;
        __nvvm_atom_cas_gen_i((int*)L.found, 0, (int)(w + 1));
    }
    st.rng = rng;
    st.nunsat = nun;
    L.walker[w] = st;
    if (f) __nvvm_atom_add_gen_ll((long long*)L.flips_total, (long long)f);
}

// ---------------------------------------------------------------------------
// One warp per walker. The 32 lanes split each occurrence-list scan (break
// counts, make and break updates), so the reads of one list are coalesced and
// a flip costs a few dependent memory round trips instead of one per list
// entry. Every lane keeps the same RNG state and nunsat, so control flow is
// warp-uniform; the unsat-list removals (rare, order-dependent) are done by
// lane 0 with a warp barrier after each.

#define PS_FULL 0xffffffffu

PS_DEV ps_u32 lane_id() { return __nvvm_read_ptx_sreg_laneid(); }
PS_DEV ps_u32 ballot(bool p) { return (ps_u32)__nvvm_vote_ballot_sync(PS_FULL, p); }
PS_DEV ps_u32 shfl(ps_u32 v, ps_u32 src) { return (ps_u32)__nvvm_shfl_sync_idx_i32(PS_FULL, (int)v, (int)src, 0x1f); }
PS_DEV ps_u32 warp_sum(ps_u32 v) { return (ps_u32)__nvvm_redux_sync_add((int)v, PS_FULL); }
PS_DEV void warp_sync() { __nvvm_bar_warp_sync(PS_FULL); }

PRISM_KERNEL void prism_probsat_walk_warp(PsLaunch L) {
    const ps_u64 w = gid() >> 5;
    const ps_u32 lane = lane_id();
    if (w >= L.walkers) return;  // whole warps only: the host launches walkers x 32 threads
    struct PsWalker st = L.walker[w];
    if (st.done) return;
    volatile ps_u32* found = L.found;
    const ps_u64 cb = w * L.num_clauses;
    ps_u32* a = L.assign + w * L.words;
    ps_u8* nt = L.ntrue + cb;
    ps_u32* wh = L.where + cb;
    ps_u32* us = L.unsat + cb;
    const ps_u32* __restrict__ cl_start = L.cl_start;
    const ps_i32* __restrict__ cl_lits = L.cl_lits;
    const ps_u32* __restrict__ occ_start = L.occ_start;
    const ps_u32* __restrict__ occ = L.occ;
    ps_u64 rng = st.rng;
    ps_u32 nun = st.nunsat;
    ps_u32 f = 0;
    for (; f < L.flips && nun != 0; ++f) {
        if ((f & 63u) == 0 && shfl(lane == 0 ? *found : 0u, 0) != 0) break;
        const ps_u32 c = us[(ps_u32)(((splitmix(rng) >> 32) * (ps_u64)nun) >> 32)];
        const ps_u32 b0 = cl_start[c], b1 = cl_start[c + 1];
        float sum = 0.0f;
        ps_i32 pick = cl_lits[b0];
        ps_u64 bits = 0;
        ps_u32 nbits = 0;
        for (ps_u32 k = b0; k < b1; ++k) {
            const ps_i32 lit = cl_lits[k];
            const ps_u32 li = lit_index(-lit);
            const ps_u32 s0 = occ_start[li], s1 = occ_start[li + 1];
            ps_u32 brk = 0;
            for (ps_u32 o = s0; o < s1 && brk < PS_WEIGHTS - 1; o += 32) {
                const ps_u32 j = o + lane;
                brk += warp_sum(j < s1 && nt[occ[j]] == 1 ? 1u : 0u);
            }
            const float wgt = L.weight[brk < PS_WEIGHTS - 1 ? brk : PS_WEIGHTS - 1];
            sum += wgt;
            if (nbits == 0) {
                bits = splitmix(rng);
                nbits = 2;
            }
            const float u = (float)((ps_u32)bits >> 8) * (1.0f / 16777216.0f);
            bits >>= 32;
            --nbits;
            if (u * sum < wgt) pick = lit;
        }
        const ps_u32 v = var_of(pick);
        if (lane == 0) a[v >> 5] ^= 1u << (v & 31u);
        const ps_u32 lt = lit_index(pick), lf = lit_index(-pick);
        // make: clauses with `pick` gain a true literal; 0 -> 1 leaves the unsat list
        for (ps_u32 o = occ_start[lt], e = occ_start[lt + 1]; o < e; o += 32) {
            const ps_u32 j = o + lane;
            ps_u32 d = 0;
            bool left = false;
            if (j < e) {
                d = occ[j];
                left = nt[d]++ == 0;
            }
            ps_u32 m = ballot(left);
            while (m) {
                const ps_u32 b = (ps_u32)__builtin_ctz(m);
                m &= m - 1;
                const ps_u32 db = shfl(d, b);
                if (lane == 0) {
                    const ps_u32 pos = wh[db], last = us[nun - 1];
                    us[pos] = last;
                    wh[last] = pos;
                }
                --nun;
                warp_sync();
            }
        }
        // break: clauses with -pick lose one; 1 -> 0 joins the unsat list (in parallel)
        for (ps_u32 o = occ_start[lf], e = occ_start[lf + 1]; o < e; o += 32) {
            const ps_u32 j = o + lane;
            bool joined = false;
            ps_u32 d = 0;
            if (j < e) {
                d = occ[j];
                joined = --nt[d] == 0;
            }
            const ps_u32 m = ballot(joined);
            if (joined) {
                const ps_u32 pos = nun + (ps_u32)__builtin_popcount(m & ((1u << lane) - 1u));
                us[pos] = d;
                wh[d] = pos;
            }
            nun += (ps_u32)__builtin_popcount(m);
        }
        warp_sync();
    }
    if (lane == 0) {
        if (nun == 0) {
            st.done = 1;
            __nvvm_atom_cas_gen_i((int*)L.found, 0, (int)(w + 1));
        }
        st.rng = rng;
        st.nunsat = nun;
        L.walker[w] = st;
        if (f) __nvvm_atom_add_gen_ll((long long*)L.flips_total, (long long)f);
    }
}
