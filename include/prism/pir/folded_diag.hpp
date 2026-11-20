#pragma once

// Clang constant-fold UB diagnostics from merged stderr (docs/PIR.md, stage lowering).

#include "prism/export.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace prism::pir {

// Diagnostics of the unit's own file; with `other`, those of every other file (headers) go there, with the file as
// clang printed it (--pir-header-fns: a header function checked in this unit).
PRISM_API std::vector<FoldedUb> parse_folded_clang_diagnostics(const std::string& clang_output,
                                                               const std::filesystem::path& src,
                                                               std::vector<std::pair<std::string, FoldedUb>>* other = nullptr);

}  // namespace prism::pir
