#pragma once

#include "prism/export.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism {

struct FunctionInfo {
    std::string file;
    std::string name;
    std::string kind;  // SCALAR | POINTER | VOID | OTHER
    int line = 0;
    std::string signature;
    std::vector<std::pair<std::string, std::string>> params;
    std::string return_type = "int";
    bool is_static = false;
    std::string body;
    std::pair<int, int> span{0, 0};
    // Source position (1-based line and byte column) of body[0], the character
    // after the opening brace; 0 when unknown (not in the JSON form). body is a
    // length-preserving copy of the source (comments blanked), so an offset
    // into it maps back to a source position (bmc nondet call sites, loops).
    int body_line = 0, body_col = 0;
    // The stripped text drops the `/*` and `*/` delimiters (4 characters per
    // comment), so a column after a block comment on the same line is left of
    // its source column: (line, stripped column, characters dropped there),
    // from comment_col_shifts, for the body's lines. Not in the JSON form.
    std::vector<std::array<int, 3>> col_shifts;
    // C++ standard of the unit (the year of its -std= in compile_commands.json,
    // 98 -> 3), 0 = not known (the compiler's default is assumed). Set by the
    // pipeline before bmc (with_cxx_std); not in the JSON form.
    int cxx_std = 0;
    // `main`'s first parameter is argc, nonnegative (C11 5.1.2.2.1p2). Set by
    // the bmc stage (program_main); not in the JSON form.
    bool argc_nonneg = false;
    // The unit names `main` somewhere other than the head of a definition of
    // main (a prototype, `&main`, `(main)(...)`, a file-scope initialiser, a
    // macro). Set by extract_functions for every function of the unit; not in
    // the JSON form. bmc then assumes nothing about main's arguments.
    bool unit_names_main = false;
    // The unit makes some name weak or an alias (`#pragma weak`, `_Pragma`,
    // a weak or alias attribute on any declaration), so a definition in it
    // may be replaced at link time without its own signature saying so. Set
    // by extract_functions; not in the JSON form.
    bool unit_has_weak = false;
};

// Source column of a (line, column) of the stripped body text.
inline int source_col(const FunctionInfo& fn, int line, int col) {
    int out = col;
    for (auto& sh : fn.col_shifts)
        if (sh[0] == line && sh[1] <= col) out += sh[2];
    return out;
}

struct Finding {
    std::string stage;
    std::string status;
    std::string file;
    std::optional<std::string> function;
    std::optional<int> line;
    std::string cls;
    std::string message;
    std::string strength;
    std::string evidence;
    std::string counterexample;
    std::map<std::string, std::string> extra;
};

struct StageResult {
    std::string name;
    std::string status;
    std::string detail;
    double started = 0;
    double elapsed = 0;
    std::vector<Finding> findings;
    int records = 0;
    std::string install;
};

struct RunReport {
    std::string root;
    double started = 0;
    std::vector<StageResult> stages;
    std::vector<FunctionInfo> functions;
    double visibility = 0;
    double answer = 0;
    double resolution = 0;
    double confidence = 0;
    std::vector<std::string> notes;

    PRISM_API std::string dumps() const;
    PRISM_API void save(const std::filesystem::path& path) const;
    PRISM_API static std::optional<RunReport> load(const std::filesystem::path& path);
};

PRISM_API Finding finding_from_json_object(const std::string& json_object);
PRISM_API StageResult stage_from_json_object(const std::string& json_object);
PRISM_API FunctionInfo function_from_json_object(const std::string& json_object);

}  // namespace prism
