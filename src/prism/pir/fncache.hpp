#pragma once

// Whole-function verdict cache (docs/UPGRADE_PLAN.md P1.2, docs/PIR.md
// "Function verdict cache"). A function whose PIR, check options, PRISM_*
// environment and engine binary are byte-identical to an earlier check gets
// that check's verdict back without encoding or solving anything.
//
// Sound by construction of the key, not by trust in the entry:
//   * the key hashes a complete serialisation of the PIR function (every
//     field of Function, Var, Stmt, Phi, Term, Block), not the display text;
//   * every CheckOptions field that can change an answer (unwind, deepening,
//     per-VC timeout, function budget, portfolio, memory encoding);
//   * every PRISM_* environment variable and its value (conservative: a
//     setting that changes nothing still gives a new key);
//   * the BLAKE3 digest of the file the engine code was loaded from (the
//     executable or the shared library), so any rebuild is a new cache.
// Only verdicts that ran to completion are stored: PROVED, PROVED-UNBOUNDED,
// PROVED-ASSUMING, BOUNDED, FAILED, NEEDS-HARNESS with no budget cut (no
// function/Houdini budget note). UNKNOWN, TIMEOUT, ERROR are never stored;
// certified runs, only_props runs and fault-injection runs bypass the cache.
// A hit says so: extra.verdict_cache = "hit <key prefix>" (Law 7).

#include "prism/pir.hpp"

#include <optional>
#include <string>

namespace prism::pir::fncache {

// "" when the cache is off for this check (see above, or PRISM_PIR_FN_CACHE=0,
// or the engine binary could not be identified).
std::string key(const Function& fn, const CheckOptions& opt);
std::optional<Verdict> get(const CheckOptions& opt, const std::string& key);
void put(const CheckOptions& opt, const std::string& key, const Verdict& v);
// The verdict is complete (not cut by a clock or memory) and may be stored.
bool storable(const Verdict& v);
// Complete serialisation used in the key (tests).
std::string fingerprint(const Function& fn);

}  // namespace prism::pir::fncache
