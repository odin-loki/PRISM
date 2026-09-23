#pragma once

// Roadmap 2.8: C/C++ lints on the Clang AST (C++ engine only, roadmap D8).
//
// The layer runs inside the `lints` stage, after the regex lints
// (checkers_*.cpp). Front end: libclang's C API, loaded at run time
// (astlint_libclang.cpp; compiled when CMake finds <clang-c/Index.h>, option
// PRISM_LIBCLANG), else clang as a process
// (`clang -fsyntax-only -Xclang -ast-dump=json`). Either way the file's flags
// come from compile_commands.json when the tree has one, and nothing from the
// scanned tree is executed (Law 9 does not apply to a parse).
//
// What is checked: each translation unit's own code, the scanned project
// headers it includes (not system headers), class members and enums nested in
// classes / namespaces, and the non-dependent code of templates (a finding in
// an expression that depends on a template parameter is dropped: dependent
// template code stays unchecked).
//
// Law 1 / Law 7: no libclang and no clang, or a unit that does not parse, is a
// NOTRUN row (extra.layer = "clang-ast") and the regex lints still run for
// that file. Where the AST layer ran, an AST finding replaces a regex finding
// of the same class on the same line (extra.supersedes = "regex"), so nothing
// is reported twice. Every AST finding carries extra.engine = "clang-ast" and
// extra.ast_backend ("libclang" / "process").

#include "prism/config.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::astlint {

inline constexpr std::string_view ENGINE = "clang-ast";
inline constexpr std::string_view LAYER_KEY = "layer";

// Classes the AST layer reports (all also in taxonomy.cpp / taxonomy.py),
// besides the regex "<fn>() return is discarded" family it ports as one check
// (src/prism/astlint_discard.inc, generated from checkers_*.cpp).
inline constexpr const char* CLASSES[] = {
    // first layer
    "CTRL-ASSIGN-COND", "MEM-SIZEOF-PTR", "INT-SIGN-CONV", "INT-ENUM-HOLE", "CTRL-SELF-ASSIGN",
    "CTRL-DEAD-STORE", "MEM-MEMSET-SWAP", "INT-DIV-TO-FLOAT",
    // straight-line flow
    "MEM-UAF", "MEM-DOUBLE-FREE", "PTR-NULL-DEREF", "CXX-USE-AFTER-MOVE", "UNINIT-READ", "UNINIT-RETURN",
    "UNINIT-BRANCH", "PTR-UNINIT", "MEM-MISMATCHED-FREE", "MEM-NEW-DELETE",
    // expressions and calls
    "CTRL-DEAD-GUARD", "INT-SHIFT-UB", "INT-BOOL-AS-BIT", "INT-TAUTOLOGY", "INT-DIV-ZERO", "FLOAT-UB",
    "INT-TRUNC", "MEM-OVERLAP", "MEM-BCOPY", "MEM-REALLOC-SELF", "FMT-STRING", "FMT-ARGS", "FMT-PERCENT-N",
    "API-GETS", "STR-OFF-BY-ONE", "STR-STRNCPY-NUL", "STR-MISSING-NUL", "API-IGNORED-ERROR",
    "API-SCANF-UNCHECKED",
    // statements and declarations
    "CTRL-FALLTHROUGH", "CTRL-EMPTY-INFINITE", "CTRL-SHADOW", "MEM-STACK-ESCAPE",
    // C++
    "CXX-NODISCARD", "CXX-VIRTUAL-IN-CTOR", "CXX-MISSING-VIRTUAL-DTOR", "CXX-SELF-MOVE", "CXX-MOVE-CONST",
    "CXX-BIND-TMP", "CXX-DANGLING-REF", "CXX-CATCH-BY-VALUE", "CXX-THROW-COPY", "CXX-THROW-DESTRUCTOR",
    "CXX-THROW-NOEXCEPT", "CXX-THROW-NEW", "CXX-DELETE-THIS",
};

struct FileResult {
    bool ran = false;                  // the unit parsed and the checks ran
    std::string error;                 // why not (clang missing, parse error, bad dump)
    std::vector<Finding> findings;     // FAILED rows, extra.engine = clang-ast
    std::vector<std::string> headers;  // report paths of project headers checked through this unit
    std::string backend;               // "libclang" / "process"
};

// Scanned project headers: canonical path -> report path. Declarations from
// these files are checked (through the units that include them).
using Headers = std::map<std::string, std::string>;

enum class Backend { Auto, LibClang, Process };

// Pure: run the AST checks on one `clang -Xclang -ast-dump=json` text.
// `main_file` is the path exactly as it was passed to clang; `source` is the
// file's bytes (offsets in the dump index into it). Declarations from other
// files are skipped without being materialised (except scanned project
// headers in `headers`), so a C++ unit that includes the standard library
// stays cheap to analyse.
FileResult analyze_dump(std::string_view dump, const std::string& main_file, std::string_view source,
                        std::string_view rel, const Headers& headers = {});

// Flags for `src` from a compile_commands.json under `root` (root/ or
// root/build/): only -I/-isystem/-iquote/-idirafter/-D/-U/-std=/-include,
// relative paths resolved against the entry's directory. Empty when there is
// no database or no entry for the file.
std::vector<std::string> compile_db_flags(const std::filesystem::path& root, const std::filesystem::path& src);

// True when libclang can be loaded; `why` says why not, `where` which library.
bool libclang_available(std::string* why = nullptr, std::string* where = nullptr);

// Parse one unit and analyse it. `clang` = nullopt means no clang binary;
// Backend::Auto uses libclang when it loads, else the clang process.
FileResult run_file(const std::optional<std::filesystem::path>& clang, const std::filesystem::path& src,
                    const std::filesystem::path& root, std::string_view rel, double timeout_s,
                    Backend backend = Backend::Process, const Headers& headers = {});

// Drop regex findings that an AST finding in the same file supersedes (same
// class, same line); the AST finding keeps the regex row's function name and
// records extra.supersedes = "regex".
void supersede(std::vector<Finding>& regex, std::vector<Finding>& ast);

// The lints stage: regex lints for every file, the AST layer for every C/C++
// translation unit, merged. By default libclang (else the clang of
// pir::find_frontend(cfg)); the overload takes the clang binary and the front
// end explicitly (tests: Backend::Process with nullopt is "no tool at all").
std::vector<Finding> run_lints_ast(const std::vector<std::filesystem::path>& paths,
                                   const std::filesystem::path& root, const Config& cfg);
std::vector<Finding> run_lints_ast(const std::vector<std::filesystem::path>& paths,
                                   const std::filesystem::path& root, const Config& cfg,
                                   const std::optional<std::filesystem::path>& clang,
                                   Backend backend = Backend::Process);

// True for a NOTRUN row that describes a sub-layer of a stage that did run
// (the stage itself is not NOTRUN because of it).
bool is_layer_row(const Finding& f);

}  // namespace prism::astlint
