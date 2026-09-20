#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

inline constexpr const char* C_EXTS[] = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", nullptr};
inline constexpr const char* TU_EXTS[] = {".c", ".cc", ".cpp", ".cxx", nullptr};

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
PRISM_API bool tu_is_empty(const std::filesystem::path& path);

}  // namespace prism
