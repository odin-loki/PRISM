// A Z3 allocation ballast (include/prism/complexity.hpp, docs/TARGETED_PROVING.md W1).
//
// Z3's QF_BV strategy (third_party/z3/src/tactic/smtlogics/qfbv_tactic.cpp)
// simplifies the bit-blasted formula with AIGs only while Z3's process-wide
// allocation count is under 300 MB (MEMLIMIT, a memory probe). So the work one
// query takes, and whether it ends within its complexity limit, depended on
// what else the process held at that moment: other functions' contexts at
// --jobs > 1 (measured: the same k-induction step, the same formula and AST
// ids, 30 730 rlimit units at --jobs 1 and 39 835 at --jobs 4). The ballast
// is one allocation Z3 counts but nobody touches (no resident memory: the
// pages are never written), taken before the first query, so the probe gives
// the same answer for every query in every process and every query takes the
// same path. PRISM's own memory checks subtract it (z3_ballast_bytes()).
//
// This file includes a Z3 internal header (util/memory_manager.h: the
// allocator the probe reads; it has no C API) and nothing else of Z3's.

#include "prism/complexity.hpp"

#ifdef PRISM_HAS_Z3
// Z3's own header, compiled with PRISM's warnings: not PRISM's code to fix
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "util/memory_manager.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#include <atomic>
#include <mutex>

namespace prism::complexity {

namespace {
std::atomic<std::uint64_t> g_ballast{0};
}

void install_z3_ballast() {
#ifdef PRISM_HAS_Z3
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            if (memory::allocate(static_cast<std::size_t>(kZ3BallastBytes)))
                g_ballast.store(kZ3BallastBytes);  // kept for the process
        } catch (...) {
            // Z3's memory_max_size below the ballast: no ballast (the probe
            // then follows the process's real allocation, as before)
        }
    });
#endif
}

std::uint64_t z3_ballast_bytes() { return g_ballast.load(); }

}  // namespace prism::complexity
