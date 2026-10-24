#pragma once

// Clang constant-fold UB diagnostics from merged stderr (docs/PIR.md, stage lowering).

#include "prism/export.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace prism::pir {

PRISM_API std::vector<FoldedUb> parse_folded_clang_diagnostics(const std::string& clang_output,
                                                               const std::filesystem::path& src);

}  // namespace prism::pir
