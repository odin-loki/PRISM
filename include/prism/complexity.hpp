#pragma once

// Complexity limits (docs/TARGETED_PROVING.md W1): every Z3 query of the pir
// stage runs under a deterministic resource limit, Z3's `rlimit`. The count
// Z3 keeps of its own work (propagations, conflicts, rewrite steps, ...) does
// not depend on the machine, its load or the number of cores, so a query that
// is "not decided within complexity C" is not decided on every machine and in
// every run. The wall-clock limits (--timeout, the function budget) stay as
// backstops, kWallBackstop times longer than the time the units stand for.
//
//   scale   rlimit units of one verification-condition query of a function
//           of reference size: --complexity N, else $PRISM_PIR_RLIMIT, else
//           --timeout seconds of kUnitsPerSecond. 0 turns the limits off
//           (wall clock only, as before).
//   units   every former time limit t of the pir stage (a VC's timeout, a
//           Houdini group's share, the Houdini search, the function budget)
//           becomes t x scale / timeout x size_factor(function) units.
//   Meter   what one function spent, per phase (extra.metrics).
//
// A query that reaches its limit is UNKNOWN (no answer), never a proof and
// never a clean result (Laws 1, 7): the limit only makes "not decided"
// reproducible. extra.complexity_limit names the first query that reached it.

#include "prism/export.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace prism::complexity {

// Units Z3 spends per second of solving on the reference machine (4-core
// x86-64, Z3 4.13.4, testdata/bench/hard; docs/TARGETED_PROVING.md): the
// default scale is --timeout seconds of that.
inline constexpr std::uint64_t kUnitsPerSecond = 1'000'000;
// Wall-clock backstops are this many times the time their units stand for
// (a loaded machine solves slower; the backstop must not be what decides).
inline constexpr double kWallBackstop = 3.0;

// --complexity N (0: off). Overrides $PRISM_PIR_RLIMIT. Process-wide.
PRISM_API void set_scale(std::uint64_t units);
// The explicit scale (--complexity, else $PRISM_PIR_RLIMIT), or -1 when unset.
PRISM_API std::int64_t explicit_scale();
// The scale of a run with this --timeout: the explicit one, else timeout x kUnitsPerSecond.
PRISM_API std::uint64_t scale_for(double timeout_s);
// The size factor of a function of `stmts` PIR statements (phis included,
// inlined callees counted): 1 + log2(1 + stmts / 4096), at most 4.
PRISM_API double size_factor(std::size_t stmts);
// Units -> the 32-bit rlimit Z3 takes (0 stays 0 = none; a positive input is at least 1).
PRISM_API unsigned z3_limit(double units);

enum class Phase : int { Encode = 0, Vcs, Unwind, KInduction, Houdini, Deepen, Count };
PRISM_API const char* phase_name(Phase p);

// Units and queries one function spent, per phase. Thread-safe: concurrent
// queries of one function charge the same meter.
struct PRISM_API Meter {
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Phase::Count)> units{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Phase::Count)> queries{};
    std::atomic<std::uint64_t> limit_hits{0};
    void add(Phase p, std::uint64_t u, bool query = true);
    std::uint64_t total() const;
    std::uint64_t total_queries() const;
    // The first query (by label) that reached its limit.
    void note_limit(const std::string& label, std::uint64_t limit);
    std::string first_limit() const;
    // Adds what another meter counted (a strategy's own meter, once its
    // answer is used); its first limit counts after this one's.
    void merge(const Meter& o);

private:
    mutable std::mutex mu_;
    std::string first_limit_;
};

// $PRISM_PIR_RLIMIT_TRACE=1: one stderr line per metered query (units, seconds).
PRISM_API bool trace();

// Z3's QF_BV strategy runs its AIG simplification only while Z3's
// process-wide allocation is under 300 MB, so the same query did different
// work depending on what else the process held (src/prism/solver/z3_ballast.cpp).
// The ballast (counted by Z3, never touched, so no resident memory) keeps
// that count above the probe for the whole process: every query takes the
// same path. Installed before the first query (solver::solve, the pir
// encoder, `prism z3-worker`); PRISM's memory checks subtract it.
inline constexpr std::uint64_t kZ3BallastBytes = std::uint64_t{320} << 20;
PRISM_API void install_z3_ballast();
PRISM_API std::uint64_t z3_ballast_bytes();  // 0 before install (or when Z3 refused it)

}  // namespace prism::complexity
