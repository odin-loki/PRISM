#pragma once

// Documents shipped with every report (roadmap 3.2 / 8.3: the trusted base;
// 6.4: every verdict in a report links to its definition). The C++ engine
// embeds docs/TRUSTED_BASE.md and docs/VERDICTS.md at build time; the Python
// engine (prism/shipdocs.py) reads the same files. Both write byte-identical
// copies next to report.md.

#include "prism/export.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

struct ShippedDoc {
    std::string_view name;  // file name written next to the reports
    std::string_view text;
};

inline constexpr std::string_view TRUSTED_BASE_FILE = "TRUSTED_BASE.md";
inline constexpr std::string_view VERDICTS_FILE = "VERDICTS.md";

PRISM_API const std::vector<ShippedDoc>& shipped_docs();
PRISM_API std::string_view trusted_base_text();
PRISM_API std::string trusted_base_sha256();
// "verdict-proved-certified" for a lattice status; "" for anything else.
PRISM_API std::string verdict_anchor(std::string_view status);
// Writes every shipped document into dir (created when missing).
PRISM_API void write_shipped_docs(const std::filesystem::path& dir);

}  // namespace prism
