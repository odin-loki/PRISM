#pragma once

// Polyglot stage internals (src/prism/polyglot.cpp): the check table, the
// built-in scans, the output parsers and the native JSON/TOML syntax
// checkers. The stage entry points are in prism/stages.hpp; this header
// exists so tests/cpp/test_polyglot.cpp can pin the table and feed canned
// tool output to the parsers.

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::polyglot {

// One syntax error found by a native checker. line/col are 1-based; 0 means
// unknown (e.g. invalid UTF-8 reported for the whole file).
struct SyntaxIssue {
    int line = 0;
    int col = 0;
    std::string msg;
};

// Native checker: nullopt = the text parses.
using NativeCheck = std::optional<SyntaxIssue> (*)(std::string_view text);

struct Tool {
    std::string name;
    std::vector<std::string> exes;
    std::vector<std::string> argv;  // {exe} {files} {file} {devnull}
    std::string pattern;
    bool per_file = false;
    std::vector<int> ok_rcs{0, 1};
    double timeout = 120.0;
    std::string cwd_marker{};
    std::string unconfigured{};
    // Runs code from the scanned tree while "checking" it (perl -c BEGIN
    // blocks, cargo build.rs / proc macros, eslint.config.js). Law 9:
    // NOTRUN without --allow-exec.
    bool executes = false;
    // Output lines (full match, stripped) meaning "ran fine, nothing to
    // report". Other output with no parsed diagnostic is ERROR "output not
    // understood", never a quiet UNKNOWN.
    std::vector<std::string> benign{};
    // Built into PRISM (no process): JSON and TOML. Always present.
    NativeCheck native = nullptr;
};

struct Check {
    std::string group;
    std::vector<std::string> languages;
    std::string kind;  // syntax | lint | type
    std::vector<Tool> tools;
    std::string install;
};

struct BuiltinScan {
    const char* cls;
    const char* pattern;
    const char* message;
};

// A line carrying this marker is a deliberate fixture (e.g. a fake key in a
// test); the built-in scan skips it.
inline constexpr const char* ALLOW_MARKER = "prism:allow";
inline constexpr std::uintmax_t MAX_FILE_BYTES = 2'000'000;
inline constexpr std::size_t SNIFF_BYTES = 8192;
inline constexpr std::size_t MAX_FILES_PER_TOOL = 2000;

PRISM_API const std::vector<Check>& checks();
PRISM_API std::span<const BuiltinScan> builtin_scans();
PRISM_API const std::map<std::string, std::string>& lang_exts();
PRISM_API const std::set<std::string>& c_family_exts();
PRISM_API const std::set<std::string>& text_only_exts();

// Conflict markers and leaked credentials in every text file; one CLEAN
// "(not a proof)" row when nothing is found.
PRISM_API std::vector<Finding> builtin_scan(const std::vector<std::filesystem::path>& files,
                                            const std::filesystem::path& root);
// Diagnostics in a tool's output (notes/info dropped). cwd: the directory
// relative file names in the output are relative to (empty = none).
PRISM_API std::vector<Finding> parse_output(const Tool& tool, const std::string& text,
                                            const std::filesystem::path& root,
                                            const std::filesystem::path& cwd, const Check& check);
// Output lines that are neither diagnostics nor the tool's benign chatter.
PRISM_API std::string unexplained_output(const Tool& tool, const std::string& raw);
// cfg.tools[name or exe] (~ expanded), the pinned ~/.prism/tools build, PATH.
PRISM_API std::optional<std::filesystem::path> resolve(const Tool& tool, const Config& cfg);

// RFC 8259 JSON (a leading UTF-8 byte order mark is allowed).
PRISM_API std::optional<SyntaxIssue> json_syntax(std::string_view text);
// TOML 1.0.0, including the table/key redefinition rules. A leading UTF-8
// byte order mark is allowed.
PRISM_API std::optional<SyntaxIssue> toml_syntax(std::string_view text);

}  // namespace prism::polyglot
