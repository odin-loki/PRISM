// Launch parameters of the GPU ProbSAT kernels (src/cuda/probsat.cu), shared
// with the host (src/prism/solver/gpu_sls.cpp). Plain C types only: the device
// side is built with -nocudainc, without any standard headers.
#pragma once

typedef unsigned char ps_u8;
typedef unsigned int ps_u32;
typedef int ps_i32;
typedef unsigned long long ps_u64;

// Number of entries of the break-weight table (eps + break)^-cb; a break of
// kPsWeights-1 or more uses the last entry (the weight is already < 1e-3 there).
#define PS_WEIGHTS 32

struct PsWalker {
    ps_u64 rng;     // splitmix64 state
    ps_u32 nunsat;  // clauses on this walker's unsat list
    ps_u32 done;    // 1: reached nunsat == 0
};

// Memory layout. The CNF is shared and read-only (CSR, literals deduplicated,
// tautologies dropped on the host). Per-walker state is walker-major and
// compact: 1 byte ntrue + 4 bytes where + 4 bytes unsat per clause, and one
// bit per variable, so 9 bytes x clauses + vars/8 per walker. The host sizes
// the walker count to the governor budget instead of a fixed 1024.
struct PsLaunch {
    const ps_u32* cl_start;   // num_clauses + 1
    const ps_i32* cl_lits;    // DIMACS literals
    const ps_u32* occ_start;  // 2 * num_vars + 3 (literal index 2|l| + (l < 0))
    const ps_u32* occ;        // clause ids per literal index
    const float* weight;      // PS_WEIGHTS entries
    ps_u32* assign;           // walkers x words (bit v of word v/32: variable v)
    ps_u8* ntrue;             // walkers x num_clauses
    ps_u32* where;            // walkers x num_clauses: position on the unsat list
    ps_u32* unsat;            // walkers x num_clauses: the unsat list
    struct PsWalker* walker;  // walkers
    ps_u32* found;            // 0, or 1 + the first walker that satisfied every clause
    ps_u64* flips_total;      // flips summed over walkers
    ps_u64 seed;
    ps_u64 base, count;       // init kernels: this chunk of the walkers x (words | clauses) range
    ps_u32 num_vars, num_clauses, words, walkers;
    ps_u32 flips;             // walk kernel: flips per walker in this launch
    ps_u32 pad;
};
