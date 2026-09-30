#pragma once

// Executing suite tasks natively under UBSan+ASan: function signatures and
// scalar ranges, drivers, the sanitizer compilers, the bwrap sandbox,
// counterexample parsing and replay, and the whole-program run of the
// deterministic esbmc-cpp tasks.
//
// Only the trusted suite in this repository, the pinned Juliet archive
// (hash-checked) and programs the scorers generate are ever compiled and run.

#include "task.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::qa {

using ojson = nlohmann::ordered_json;
using Params = std::vector<std::pair<std::string, std::string>>;  // (type, name)

extern const std::vector<std::string> SAN_FLAGS;
extern const std::vector<std::pair<std::string, std::string>> SAN_ENV;

// Value type of a parameter: qualifiers dropped, `T&` read as `T`.
std::string norm_type(const std::string& t);
std::optional<std::pair<i128, i128>> type_range(const std::string& t);
// (return type, [(param type, param name)]) of fn's definition
std::optional<std::pair<std::string, Params>> find_signature(const std::string& text, const std::string& fn);
std::string c_literal(i128 v, const std::string& typ);
// the parameters when every one is an integer scalar, else nothing
std::optional<Params> scalar_params(const Task& task, const std::string& fn);
std::string driver_source(const Task& task, const std::string& fn, const Params& params,
                          const std::vector<std::vector<i128>>& rows);

bool bwrap_ok();
// (C compiler, C++ compiler) that can link UBSan+ASan: clang, else gcc
// ($CC_SAN / $CXX_SAN override both)
std::pair<std::string, std::string> sanitizer_compilers();
std::vector<std::string> compiler_for(const std::string& lang);

struct Exec {
    std::string outcome;  // ub | clean | compile-error | timeout | crash
    std::string detail;
};

Exec run_sanitized(const Task& task, const std::string& fn, const Params& params,
                   const std::vector<std::vector<i128>>& rows, const fs::path& work, double timeout = 60.0);
// Compile a whole-program task natively (sanitizers, assertions on) and run main.
Exec run_program(const Task& task, const fs::path& work, double timeout = 60.0);

std::vector<i128> edge_values(i128 lo, i128 hi);
std::vector<std::vector<i128>> input_grid(const Params& params, int n_random = 2000, long long seed = 7);

// `a=1, b=-2` or `a=#x0000000f` (Z3 bit-vectors: raw bit patterns, which
// replay reinterprets in the parameter's C type); in match order, a repeated
// name keeps its last value
std::vector<std::pair<std::string, i128>> parse_cex(const std::string& cex);
ojson replay(const Task& task, const std::string& fn, const std::string& cex, const fs::path& work);

// Python helpers shared by the reports
std::string py_float(double v);          // repr(float)
std::string utf8_clean(const std::string& s);  // invalid bytes -> U+FFFD (errors="replace")
ojson int_json(i128 v);                  // JSON integer (int64 / uint64 range)
std::string path_key(const std::string& ident);  // ident with '/' -> "__"

}  // namespace prism::qa
