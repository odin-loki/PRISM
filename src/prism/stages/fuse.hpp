#pragma once

// Internal to prism_core: the concrete and binary fuzzer of stage fuse
// (src/prism/stages/fuse.cpp).

#include "interp.hpp"

namespace prism::stages_detail {

// One counterexample value: decimal (optionally signed), 0x hex, or a
// bitvector numeral (#x.. / #b..). A value above INT_MAX (an unsigned
// counterexample such as 4294967295) is kept; a decimal with a leading zero
// is ambiguous (C octal or decimal) and is rejected.
std::optional<uint64_t> cex_value(std::string v);

// "x=5, y=-1" as the fuzzer's input bytes: each parameter little-endian at
// its own width (the decode_args / harness layout). Values name their
// parameter; values whose name matches no parameter are read in order.
std::optional<std::vector<uint8_t>> bytes_from_cex(const std::string& cex, const FunctionInfo& fn);

// Concrete-oracle fuzzing of one function, then (with --allow-exec) the
// sanitized binary harness. CRASH, CLEAN (not a proof), NEEDS-HARNESS, or
// ERROR when the concrete interpreter could not run the function.
Finding fuzz_function(const FunctionInfo& fn, const std::filesystem::path& src, double budget, int iters,
                      const std::vector<std::vector<uint8_t>>* seeds);

}  // namespace prism::stages_detail
