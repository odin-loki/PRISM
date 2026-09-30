#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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
// Where a delimiter-dropping strip removes block-comment delimiters: (line,
// output column of the next kept character, characters dropped there).
// strip_comments_keep_lines itself is length-preserving (drops nothing).
PRISM_API std::vector<std::array<int, 3>> comment_col_shifts(std::string_view text);
PRISM_API int match_brace(std::string_view text, int open_idx);
PRISM_API bool body_returns_local_array(std::string_view body);
PRISM_API bool body_needs_pointer_harness(std::string_view body);
PRISM_API std::vector<FunctionInfo> extract_functions(const std::filesystem::path& path,
                                                      std::string rel = {});
// extract_functions on source text already read (`rel` names its file).
PRISM_API std::vector<FunctionInfo> extract_functions_from_text(std::string_view text, const std::string& rel);
// Each byte of an invalid UTF-8 sequence as SCRUBBED_BYTE. Every source is
// read through this: the regex front end and the JSON reports take UTF-8
// only, and one byte for one keeps byte offsets (columns) where they were.
// The replacement changes the program (a Latin-1 'é' in a character literal
// is -23, the replacement is 127), so no engine models a body that holds
// one: see scrubbed_byte_reason.
inline constexpr char SCRUBBED_BYTE = '\x7f';
PRISM_API std::string scrub_utf8(std::string_view text);
// "UNENCODED: ..." when `body` holds SCRUBBED_BYTE (a scrubbed byte, or a
// raw DEL, which no engine models either): every semantic engine (bmc,
// interval, wp, concolic, fuzzers, diff, rapid) turns that into
// NEEDS-HARNESS or no verdict, never a proof or a refutation. The check is
// on the text, so it survives inlining and the functions.json journal.
PRISM_API std::optional<std::string> scrubbed_byte_reason(std::string_view body, std::string_view engine);
PRISM_API std::vector<std::filesystem::path> iter_sources(const std::filesystem::path& root);
// Top-level brace-delimited code no parsed function owns (Law 7): (line,
// first line of the text before its `{`). Nothing checks that code.
PRISM_API std::vector<std::pair<int, std::string>> parse_gaps(const std::filesystem::path& path);
PRISM_API std::vector<std::pair<int, std::string>> parse_gaps_from_text(std::string_view text);
// parse_gaps as inventory records: NOTRUN, cls PARSE-GAP, one per gap.
PRISM_API std::vector<Finding> parse_gap_findings(const std::filesystem::path& path,
                                                  const std::string& rel);
PRISM_API bool tu_is_empty(const std::filesystem::path& path);

}  // namespace prism
