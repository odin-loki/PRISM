#pragma once

// Machine-readable output of the pir stage (--jsonl FILE; docs/PIR.md
// "Machine-readable output"): one JSON line per function with the
// OS-Proving key, the verdict, the proof level, the assumptions, time and
// memory, and whether the function is inside the Lean-proved LLVM fragment.

#include "prism/config.hpp"
#include "prism/models.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace prism::pir {

// Proof level of a finding: "L0".."L3", or "" when the function did not even
// compile. L0 compiled (front end, IR and translation attempted), L1 bounded
// (BOUNDED), L2 unbounded runtime-error freedom (PROVED, PROVED-UNBOUNDED,
// PROVED-CERTIFIED, PROVED-ASSUMING). L3 (contract) is never produced by pir.
std::string proof_level(const Finding& f);

// "memory-limit" when the function's check stopped at PRISM's memory budget
// (UNKNOWN with a memory-budget message), else "".
bool hit_memory_limit(const Finding& f);

// One record (schema prism-pir-jsonl/1).
nlohmann::json jsonl_record(const Finding& f, const Config& cfg);

// Appends one line per finding to cfg.jsonl in a single write, and rewrites
// <cfg.jsonl>.summary.json with the counts of this run. No-op without --jsonl.
void write_jsonl(const Config& cfg, const std::vector<Finding>& findings);

}  // namespace prism::pir
