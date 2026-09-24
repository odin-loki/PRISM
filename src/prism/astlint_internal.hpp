#pragma once

// Internal interface of the Clang-AST lint layer (roadmap 2.8).
//
// Two front ends produce the same thing: a list of top-level declarations in
// the schema of `clang -Xclang -ast-dump=json` (nlohmann JSON), restricted to
// the files the layer checks (the unit's main file and the scanned project
// headers it includes). The checks (astlint_checks.cpp) only see that schema.
//
//   * astlint.cpp: clang as a process, the JSON dump split without a full
//     parse (the fallback, and the only path when libclang is absent);
//   * astlint_libclang.cpp: libclang's C API loaded at run time (dlopen), the
//     cursor tree of the checked files converted to the same schema.
//
// Positions: every location object carries "_f" (the canonical path of its
// file) and "offset" (a byte offset into that file); a location inside a macro
// expansion is wrapped as {"expansionLoc": {...}}.

#include "prism/astlint.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace prism::astlint::detail {

using json = nlohmann::ordered_json;

// Facts about a record type (for delete through a base pointer).
struct RecordFacts {
    bool polymorphic = false;
    bool virtual_dtor = false;
    bool is_final = false;
    std::vector<std::string> bases;  // normalised names (dump path: resolved later)
};

// Facts about a declaration referenced from checked code (function, method).
struct DeclFacts {
    bool nodiscard = false;
    bool noreturn = false;
    bool is_virtual = false;
    bool user = false;       // declared (not implicitly) in a checked file
    std::string result;      // "lvalue" / "xvalue" / "prvalue" (call result category)
};

struct Unit {
    std::vector<json> decls;                     // top-level declarations of checked files
    std::map<std::string, DeclFacts> facts;      // decl id -> facts
    std::map<std::string, RecordFacts> records;  // normalised record name -> facts
    bool cxx = false;
    std::string backend;                         // "libclang" / "process"
};

// The files whose declarations are checked, by canonical path.
struct FileSet {
    std::string main;                          // canonical path of the unit
    std::string main_rel;                      // its report path
    std::map<std::string, std::string> rel;    // canonical project header -> report path
    std::map<std::string, std::string> sources;  // canonical path -> bytes, when given (else read)
    bool wanted(const std::string& canon) const { return canon == main || rel.contains(canon); }
    std::string rel_of(const std::string& canon) const {
        if (canon == main) return main_rel;
        auto it = rel.find(canon);
        return it == rel.end() ? std::string() : it->second;
    }
};

std::string canon_path(const std::filesystem::path& p);

// Run every check on a converted unit. `sources` maps canonical paths of
// checked files to their bytes (read on demand when missing).
FileResult analyze_unit(Unit& u, const FileSet& files);

// Normalised record name of a type spelling ("const struct ns::B *" -> "ns::B").
std::string record_name(std::string t);

// ---------------------------------------------------------------- libclang
// Compiled only with PRISM_HAS_LIBCLANG (CMake option PRISM_LIBCLANG); the
// library itself is loaded at run time, so a missing libclang.so is a clean
// fallback to the process path, never a failure to start PRISM.

// True when libclang could be loaded; `why` says why not (not built in, not
// found, missing symbols). `clang` is a clang binary to look next to.
bool libclang_load(const std::optional<std::filesystem::path>& clang, std::string* why,
                   std::string* where = nullptr);

// Parse `src` with `args` and convert the checked files. nullopt + `err` when
// the unit does not parse (first error) or libclang failed.
std::optional<Unit> libclang_unit(const std::vector<std::string>& args, const std::string& src,
                                  const FileSet& files, bool cxx, std::string& err);

}  // namespace prism::astlint::detail
