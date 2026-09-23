#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism {

inline constexpr const char* C_EXTS[] = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".i", ".ii", nullptr};
// .i / .ii are preprocessed C / C++ translation units (cc -E output).
inline constexpr const char* TU_EXTS[] = {".c", ".cc", ".cpp", ".cxx", ".i", ".ii", nullptr};

PRISM_API bool is_c_ext(std::string_view ext);
PRISM_API bool is_tu_ext(std::string_view ext);

PRISM_API std::string strip_comments_keep_lines(std::string_view text,
                                                bool blank_strings = true);
PRISM_API int match_brace(std::string_view text, int open_idx);
PRISM_API bool body_returns_local_array(std::string_view body);
PRISM_API bool body_needs_pointer_harness(std::string_view body);
PRISM_API std::vector<FunctionInfo> extract_functions(const std::filesystem::path& path,
                                                      std::string rel = {});
PRISM_API std::vector<std::filesystem::path> iter_sources(const std::filesystem::path& root);
// Top-level brace-delimited code no parsed function owns (Law 7): (line,
// first line of the text before its `{`). Nothing checks that code.
PRISM_API std::vector<std::pair<int, std::string>> parse_gaps(const std::filesystem::path& path);
// parse_gaps as inventory records: NOTRUN, cls PARSE-GAP, one per gap.
PRISM_API std::vector<Finding> parse_gap_findings(const std::filesystem::path& path,
                                                  const std::string& rel);
PRISM_API bool tu_is_empty(const std::filesystem::path& path);

}  // namespace prism
