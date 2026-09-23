#pragma once

// Roadmap 2.8: C/C++ lints on the Clang AST (C++ engine only, roadmap D8).
//
// The layer runs inside the `lints` stage, after the regex lints
// (checkers_*.cpp). Clang runs as a process
// (`clang -fsyntax-only -Xclang -ast-dump=json`), with the file's flags from
// compile_commands.json when the tree has one; nothing from the scanned tree
// is executed (Law 9 does not apply to a parse).
//
// Law 1 / Law 7: no clang, or a unit that does not parse, is a NOTRUN row
// (extra.layer = "clang-ast") and the regex lints still run for that file.
// Where the AST layer ran, an AST finding replaces a regex finding of the same
// class on the same line (extra.supersedes = "regex"), so nothing is reported
// twice. Every AST finding carries extra.engine = "clang-ast".

#include "prism/config.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::astlint {

inline constexpr std::string_view ENGINE = "clang-ast";
inline constexpr std::string_view LAYER_KEY = "layer";

// Classes the AST layer reports (all also in taxonomy.cpp / taxonomy.py).
inline constexpr const char* CLASSES[] = {
    "CTRL-ASSIGN-COND", "MEM-SIZEOF-PTR", "INT-SIGN-CONV",   "INT-ENUM-HOLE",
    "CTRL-SELF-ASSIGN", "CTRL-DEAD-STORE", "MEM-MEMSET-SWAP", "INT-DIV-TO-FLOAT",
};

struct FileResult {
    bool ran = false;           // the unit parsed and the checks ran
    std::string error;          // why not (clang missing, parse error, bad dump)
    std::vector<Finding> findings;  // FAILED rows, extra.engine = clang-ast
};

// Pure: run the AST checks on one `clang -Xclang -ast-dump=json` text.
// `main_file` is the path exactly as it was passed to clang; `source` is the
// file's bytes (offsets in the dump index into it). Declarations from other
// files (headers) are skipped without being materialised, so a C++ unit that
// includes the standard library stays cheap to parse.
FileResult analyze_dump(std::string_view dump, const std::string& main_file,
                        std::string_view source, std::string_view rel);

// Flags for `src` from a compile_commands.json under `root` (root/ or
// root/build/): only -I/-isystem/-iquote/-idirafter/-D/-U/-std=/-include,
// relative paths resolved against the entry's directory. Empty when there is
// no database or no entry for the file.
std::vector<std::string> compile_db_flags(const std::filesystem::path& root,
                                          const std::filesystem::path& src);

// Run clang on one unit and analyse it. `clang` = nullopt means not found.
FileResult run_file(const std::optional<std::filesystem::path>& clang,
                    const std::filesystem::path& src, const std::filesystem::path& root,
                    std::string_view rel, double timeout_s);

// Drop regex findings that an AST finding in the same file supersedes (same
// class, same line); the AST finding keeps the regex row's function name and
// records extra.supersedes = "regex".
void supersede(std::vector<Finding>& regex, std::vector<Finding>& ast);

// The lints stage: regex lints for every file, the AST layer for every C/C++
// translation unit, merged. `clang` overrides discovery (tests); by default the
// clang of pir::find_frontend(cfg) is used.
std::vector<Finding> run_lints_ast(const std::vector<std::filesystem::path>& paths,
                                   const std::filesystem::path& root, const Config& cfg);
std::vector<Finding> run_lints_ast(const std::vector<std::filesystem::path>& paths,
                                   const std::filesystem::path& root, const Config& cfg,
                                   const std::optional<std::filesystem::path>& clang);

// True for a NOTRUN row that describes a sub-layer of a stage that did run
// (the stage itself is not NOTRUN because of it).
bool is_layer_row(const Finding& f);

}  // namespace prism::astlint
