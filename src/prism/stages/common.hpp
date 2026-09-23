#pragma once

// Internal to prism_core: helpers shared by the stage implementations in
// src/prism/stages/*.cpp (not installed API). A helper used by one stage only
// stays file-local (anonymous namespace) in that stage's .cpp.
//
//   common.cpp    string/regex helpers, findings, source lookup, ACSL spec
//                 comments, argument decoding and seeds, bmc_one
//   platform.cpp  run_argv (process spawning) and the plain-HTTP client
//   contracts.cpp bmc_with_assume (also used by wp.cpp)
//   interp.hpp    the concrete C interpreter (interp.cpp)
//   llm.hpp       the LLM engine and the sandboxed run of LLM-written C (llm.cpp)

#include "prism/stages.hpp"
#include "prism/ai.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/regex.hpp"
#include "prism/sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace prism::stages_detail {

inline constexpr int WIDTH = 32;
inline constexpr int64_t INT_MIN_32 = -(int64_t{1} << (WIDTH - 1));
inline constexpr int64_t INT_MAX_32 = (int64_t{1} << (WIDTH - 1)) - 1;
inline constexpr int64_t INT64_MIN_V = std::numeric_limits<int64_t>::min();
inline constexpr int64_t INT64_MAX_V = std::numeric_limits<int64_t>::max();
inline constexpr int MAX_STEPS = 10000;

inline const std::map<std::string, int> kCTypeSize = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"unsigned short", 2},
    {"int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 8}, {"unsigned long", 8},
    {"long long", 8}, {"unsigned long long", 8},
    {"int8_t", 1}, {"uint8_t", 1}, {"int16_t", 2}, {"uint16_t", 2},
    {"int32_t", 4}, {"uint32_t", 4}, {"int64_t", 8}, {"uint64_t", 8},
    {"size_t", 8}, {"ssize_t", 8}, {"bool", 1}, {"_Bool", 1},
};

struct ParseFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Args = std::map<std::string, int>;

// ---- common.cpp: text helpers
std::string strip(std::string s);
std::string lstrip(std::string s);
std::string lower_copy(std::string s);
std::string join_sv(const std::vector<std::string>& v, std::string_view sep);
bool starts_kw(std::string_view text, std::string_view kw);
std::optional<Match> match_at(const Regex& re, std::string_view s);
bool fullmatch(const Regex& re, std::string_view s);
int32_t i32(int64_t x);
bool truth(int64_t v);
std::vector<std::string> split_comma(const std::string& s);
std::pair<std::string, std::string> paren(std::string text);
std::pair<std::string, std::string> brace(std::string text);
std::pair<std::string, std::string> stmt(const std::string& text);

// ---- common.cpp: findings
Finding make_find(std::string stage, std::string_view status, const FunctionInfo& fn, std::string cls,
                  std::string msg, std::string_view strength);
Finding nr(std::string stage, std::string msg, std::string strength = std::string(laws::STRENGTH_READS));

// ---- common.cpp: source files and tools
std::string read_text_file(const std::filesystem::path& p);
std::optional<std::filesystem::path> locate_source(const FunctionInfo& fn);
std::string read_fn_source(const FunctionInfo& fn);
std::optional<std::filesystem::path> which_cc();

// ---- common.cpp: argument encoding and seeds
int param_nbytes(const std::vector<std::pair<std::string, std::string>>& params);
std::map<std::string, int> decode_args(const FunctionInfo& fn, const std::vector<uint8_t>& data);
std::vector<std::vector<uint8_t>> interesting_seeds(const FunctionInfo& fn);

// ---- common.cpp: BMC of one function; ACSL / comment contracts
Finding bmc_one(const FunctionInfo& fn, int unwind);

struct Spec {
    std::vector<std::string> requires_;
    std::vector<std::string> ensures;
    std::vector<std::string> invariant;
    std::vector<std::string> decreases;
    std::optional<std::string> diff;
};
Spec parse_comments(const FunctionInfo& fn);

// ---- contracts.cpp
Finding bmc_with_assume(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                        const std::optional<std::string>& ensures, const std::optional<std::string>& decreases,
                        const std::optional<std::string>& invariant);

// ---- platform.cpp: processes and plain HTTP
struct ProcRun {
    int rc = -1;
    std::string out;
    std::string err;
    bool timeout = false;
    bool crashed = false;
};
// limits: rlimits applied in the forked child (Law 9 sandbox for built
// binaries; the caller wraps argv with sandbox::wrap_argv). Default: none.
ProcRun run_argv(const std::vector<std::string>& args, const std::string& input, double timeout_s,
                 const sandbox::Limits& limits = sandbox::Limits());
std::optional<std::string> http_request(const std::string& method, const std::string& url, const std::string& body,
                                        int timeout_ms);
bool http_ok(const std::string& url, int timeout_ms = 1500);

}  // namespace prism::stages_detail
