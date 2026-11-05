// Differential test of the std::expected model header
// (src/prism/pir/models/cxx/expected). Same shape as tests/cpp/test_cxx_list.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/expected.md has the line that links this file.
//
// A success value() is the stored T. value() / operator* on an unexpected are
// a __prism_check, and the model does not print a fake T. error() on a success
// value is a __prism_check, and the model does not print a fake E.
// and_then on a value returns the function's expected; on an unexpected it
// does not call the function and keeps that E. or_else on an unexpected
// returns the function's expected; on a value it does not call the function
// and keeps that T. Neither path invents a default T or a default E.
// value_or on a value returns that T. value_or on an unexpected returns the
// argument converted to T, not a default T and not the stored E.
// transform on a value returns an expected holding the function's result.
// transform on an unexpected does not call the function and keeps that E.
// It does not construct a T and does not return T{}.
// transform_error on an unexpected returns an expected holding the function's
// result as the error. transform_error on a value does not call the function
// and keeps that T. It does not construct an E and does not return E{} or T{}.
// operator== of the same type returns false when the alternatives differ,
// before either object is read. Both values compare T. Both errors compare E.
// operator!= is that negation. operator< returns false for a value against an
// error and true for an error against a value, before storage is read.
// emplace of one argument destroys the active alternative and placement-news T.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

struct ExpCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

ExpCtx& ctx() {
    static ExpCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <expected>\n"
                                  "int main() { std::expected<int, int> e(1); return e.value() - 1; }\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("expected_trace.cpp"), "expected_trace", cxxh::asan_flags(), with_models);
}

bool failed_is_check(const fs::path& exe, const char* mode, const char* cls, const char* which) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    const std::string needle = std::string("CHECK ") + cls + ":";
    if (both.find(needle) == std::string::npos) return false;
    if (both.find(which) == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            if (i + 1 < s.size()) i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// The function whose signature starts at sig. The next character after the
// signature in this header is '{'.
std::string function_body(const std::string& src, std::size_t sig) {
    const auto brace = src.find('{', sig);
    if (brace == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    return {};
}

int count_of(const std::string& s, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos; at += needle.size()) ++n;
    return n;
}

bool has_line(const std::vector<std::string>& lines, const char* s) {
    for (const auto& line : lines)
        if (line == s) return true;
    return false;
}

std::size_t line_at(const std::vector<std::string>& lines, const char* s) {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i] == s) return i;
    return lines.size();
}

std::size_t line_nth(const std::vector<std::string>& lines, const char* s, int n) {
    int seen = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == s) {
            if (seen == n) return i;
            ++seen;
        }
    }
    return lines.size();
}

int count_line(const std::vector<std::string>& lines, const char* s) {
    int n = 0;
    for (const auto& line : lines)
        if (line == s) ++n;
    return n;
}

bool syntax_only(const std::string& cxx, const fs::path& src, bool with_models, std::string* err = nullptr) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only", "-w"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv, nullptr, err);
}

fs::path build_src(const std::string& cxx, const fs::path& tmp, const fs::path& src, bool with_models,
                   const std::string& stem) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (stem + (with_models ? ".model" : ".lib"));
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

}  // namespace

TEST_CASE("cxx expected model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxexpected-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx expected model: success value() is the stored T; value() and error() checks") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "expected_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "expected").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 5);
    CHECK(a == b);
    const char* eq_lines[] = {
        "eq_val 1 0 1", "ne_val 0 1 0", "eq_zero 1 0", "eq_err 1 0 1", "ne_err 0 1 0", "eq_err0 1 0",
        "eq_mix 0 0",   "ne_mix 1 1",   "eq_mix0 0 0", "eq_const 1 0 1 0", "eqv 3 3", "eqv 3 4", "eq_t 1 0",
        "eqe 5 5",      "eqe 5 6",      "eq_e 1 0",    "eq_cross 0 0",     "ne_t 0",  "ne_e 1",  "ne_cross 1",
    };
    for (const char* line : eq_lines) CHECK(has_line(b, line));
    CHECK(count_line(b, "eqv 3 3") == 2);
    CHECK(count_line(b, "eqv 3 4") == 1);
    CHECK(count_line(b, "eqe 5 5") == 1);
    CHECK(count_line(b, "eqe 5 6") == 2);
    CHECK(line_nth(b, "eqv 3 3", 0) < line_at(b, "eq_t 1 0"));
    CHECK(line_at(b, "eq_cross 0 0") < line_nth(b, "eqv 3 3", 1));
    CHECK(line_nth(b, "eqv 3 3", 1) < line_at(b, "ne_t 0"));
    CHECK(line_at(b, "eq_e 1 0") < line_nth(b, "eqe 5 6", 1));
    CHECK(line_nth(b, "eqe 5 6", 1) < line_at(b, "ne_e 1"));
    const char* em_lines[] = {
        "start 1", "one 1 2 1", "mut 6",     "two 1 3 1", "emheld 0 4", "from_err 1 5 1", "lv 3 3 1",
        "rv -1 3 1", "zero 1 0 1", "zmut 4", "nine 9 9",  "box 1 42 1", "box2 1 7 1",     "wasnow 3 3",
    };
    for (const char* line : em_lines) CHECK(has_line(b, line));
    CHECK(line_at(b, "mvc2") < line_at(b, "mvd1"));
    CHECK(line_at(b, "mvd1") < line_at(b, "mvm2"));
    CHECK(line_at(b, "mvm2") < line_at(b, "one 1 2 1"));
    CHECK(line_at(b, "mvc5") < line_at(b, "erd4"));
    CHECK(line_at(b, "erd4") < line_at(b, "mvm5"));
    CHECK(line_at(b, "mvm5") < line_at(b, "from_err 1 5 1"));
    CHECK(line_at(b, "erd8") < line_at(b, "bxc42"));
    CHECK(line_at(b, "bxc42") < line_at(b, "bxd42"));
    CHECK(line_at(b, "bxd42") < line_at(b, "bxc7"));
    for (const auto& line : b) {
        CHECK(line.find("eqv 8") == std::string::npos);
        CHECK(line.find("eqe 2") == std::string::npos);
        CHECK(line != "eq_cross 1 0");
        CHECK(line != "eq_cross 0 1");
        CHECK(line != "eq_cross 1 1");
        CHECK(line != "ne_cross 0");
        CHECK(line != "one 1 2 0");
        CHECK(line != "one 0 2 1");
        CHECK(line != "from_err 0 5 1");
        CHECK(line != "from_err 1 4 1");
        CHECK(line != "from_err 1 5 0");
        CHECK(line != "zero 0 0 1");
        CHECK(line != "zero 1 0 0");
        CHECK(line != "box 0 42 1");
        CHECK(line != "box 1 42 0");
        CHECK(line != "wasnow 42 42");
        CHECK(line != "wasnow 3 42");
        CHECK(line != "lv -1 3 1");
        CHECK(line != "rv 3 3 1");
    }
    bool saw_stored = false;
    bool saw_error = false;
    bool saw_and_then = false;
    bool saw_and_then_unex = false;
    bool saw_or_else = false;
    bool saw_or_else_unex = false;
    bool saw_and_then_not = false;
    bool saw_or_else_not = false;
    bool saw_got = false;
    bool saw_and_then_val = false;
    bool saw_or_else_e = false;
    bool saw_and_then_r = false;
    bool saw_and_then_const = false;
    bool saw_or_else_const = false;
    bool saw_and_then_rv = false;
    bool saw_or_else_rv = false;
    bool saw_and_then_rv_unex = false;
    bool saw_or_else_rv_val = false;
    bool saw_and_then_cmv = false;
    bool saw_or_else_cmv = false;
    bool saw_value_or = false;
    bool saw_value_or_unex = false;
    bool saw_value_or_rv = false;
    bool saw_value_or_rv_unex = false;
    bool saw_value_or_zero = false;
    bool saw_value_or_const = false;
    bool saw_value_or_const_unex = false;
    bool saw_value_or_held = false;
    bool saw_value_or_none = false;
    bool saw_transform = false;
    bool saw_transform_unex = false;
    bool saw_transform_src = false;
    bool saw_transform_zero = false;
    bool saw_transform_err0 = false;
    bool saw_transform_const = false;
    bool saw_transform_const_unex = false;
    bool saw_transform_rv = false;
    bool saw_transform_rv_unex = false;
    bool saw_transform_cmv_val = false;
    bool saw_transform_cmv = false;
    bool saw_transform_not = false;
    bool saw_xform = false;
    bool saw_transform_val = false;
    bool saw_transform_box = false;
    bool saw_transform_box_unex = false;
    bool saw_transform_r = false;
    bool saw_transform_error = false;
    bool saw_transform_error_src_err = false;
    bool saw_transform_error_val = false;
    bool saw_transform_error_src = false;
    bool saw_transform_error_zero = false;
    bool saw_transform_error_err0 = false;
    bool saw_transform_error_zero_err = false;
    bool saw_transform_error_const_val = false;
    bool saw_transform_error_const = false;
    bool saw_transform_error_rv_val = false;
    bool saw_transform_error_rv = false;
    bool saw_transform_error_cmv_val = false;
    bool saw_transform_error_cmv = false;
    bool saw_transform_error_not = false;
    bool saw_transform_error_held = false;
    bool saw_transform_error_box = false;
    bool saw_transform_error_box_val = false;
    bool saw_terr = false;
    bool saw_transform_error_r = false;
    bool saw_transform_error_kept = false;
    bool saw_transform_error_cat = false;
    bool saw_transform_error_cat_const = false;
    bool saw_transform_error_cat_rv = false;
    bool saw_transform_error_cat_cmv = false;
    for (const auto& line : b) {
        if (line == "engaged 1 1 42 42") saw_stored = true;
        if (line == "unex 0 0 7") saw_error = true;
        if (line == "and_then 1 84") saw_and_then = true;
        if (line == "and_then_unex 0 0 9") saw_and_then_unex = true;
        if (line == "or_else 0 1 42") saw_or_else = true;
        if (line == "or_else_unex 1 11") saw_or_else_unex = true;
        if (line == "and_then_not 0 4") saw_and_then_not = true;
        if (line == "or_else_not 1 42") saw_or_else_not = true;
        if (line == "got 6") saw_got = true;
        if (line == "and_then_val 6") saw_and_then_val = true;
        if (line == "or_else_e 1 2") saw_or_else_e = true;
        if (line == "and_then_r 0 3") saw_and_then_r = true;
        if (line == "and_then_const 45") saw_and_then_const = true;
        if (line == "or_else_const 10") saw_or_else_const = true;
        if (line == "and_then_rv 15") saw_and_then_rv = true;
        if (line == "or_else_rv 7") saw_or_else_rv = true;
        if (line == "and_then_rv_unex 0 6") saw_and_then_rv_unex = true;
        if (line == "or_else_rv_val 1 8") saw_or_else_rv_val = true;
        if (line == "and_then_cmv 0 4") saw_and_then_cmv = true;
        if (line == "or_else_cmv 1 8") saw_or_else_cmv = true;
        if (line == "value_or 42") saw_value_or = true;
        if (line == "value_or_unex 99") saw_value_or_unex = true;
        if (line == "value_or_rv 42") saw_value_or_rv = true;
        if (line == "value_or_rv_unex 99") saw_value_or_rv_unex = true;
        if (line == "value_or_zero 0") saw_value_or_zero = true;
        if (line == "value_or_const 42") saw_value_or_const = true;
        if (line == "value_or_const_unex 99") saw_value_or_const_unex = true;
        if (line == "value_or_held 42") saw_value_or_held = true;
        if (line == "value_or_none 99") saw_value_or_none = true;
        if (line == "transform 1 84") saw_transform = true;
        if (line == "transform_unex 0 0 9") saw_transform_unex = true;
        if (line == "transform_src 9") saw_transform_src = true;
        if (line == "transform_zero 1 0") saw_transform_zero = true;
        if (line == "transform_err0 0 0") saw_transform_err0 = true;
        if (line == "transform_const 45") saw_transform_const = true;
        if (line == "transform_const_unex 0 0 3") saw_transform_const_unex = true;
        if (line == "transform_rv 15") saw_transform_rv = true;
        if (line == "transform_rv_unex 0 6") saw_transform_rv_unex = true;
        if (line == "transform_cmv_val 1 9") saw_transform_cmv_val = true;
        if (line == "transform_cmv 0 4") saw_transform_cmv = true;
        if (line == "transform_not 0 4") saw_transform_not = true;
        if (line == "xform 6") saw_xform = true;
        if (line == "transform_val 1 6") saw_transform_val = true;
        if (line == "transform_box 42") saw_transform_box = true;
        if (line == "transform_box_unex 0 0 9") saw_transform_box_unex = true;
        if (line == "transform_r 0 3") saw_transform_r = true;
        if (line == "transform_error 0 18") saw_transform_error = true;
        if (line == "transform_error_src_err 9") saw_transform_error_src_err = true;
        if (line == "transform_error_val 0 1 42") saw_transform_error_val = true;
        if (line == "transform_error_src 42") saw_transform_error_src = true;
        if (line == "transform_error_zero 1 0") saw_transform_error_zero = true;
        if (line == "transform_error_err0 0 99") saw_transform_error_err0 = true;
        if (line == "transform_error_zero_err 0 0") saw_transform_error_zero_err = true;
        if (line == "transform_error_const_val 42") saw_transform_error_const_val = true;
        if (line == "transform_error_const 0 10") saw_transform_error_const = true;
        if (line == "transform_error_rv_val 11") saw_transform_error_rv_val = true;
        if (line == "transform_error_rv 0 7") saw_transform_error_rv = true;
        if (line == "transform_error_cmv_val 1 8") saw_transform_error_cmv_val = true;
        if (line == "transform_error_cmv 0 5") saw_transform_error_cmv = true;
        if (line == "transform_error_not 0 9") saw_transform_error_not = true;
        if (line == "transform_error_held 1 6") saw_transform_error_held = true;
        if (line == "transform_error_box 9") saw_transform_error_box = true;
        if (line == "transform_error_box_val 0 42") saw_transform_error_box_val = true;
        if (line == "terr 3") saw_terr = true;
        if (line == "transform_error_r 0 4") saw_transform_error_r = true;
        if (line == "transform_error_kept 1 42") saw_transform_error_kept = true;
        if (line == "transform_error_cat 11") saw_transform_error_cat = true;
        if (line == "transform_error_cat_const 21") saw_transform_error_cat_const = true;
        if (line == "transform_error_cat_rv 31") saw_transform_error_cat_rv = true;
        if (line == "transform_error_cat_cmv 41") saw_transform_error_cat_cmv = true;
        CHECK(line.find("engaged 1 1 0 0") == std::string::npos);
        CHECK(line != "held 0");
        CHECK(line != "copy 0");
        CHECK(line != "err 0");
        CHECK(line != "unex 0 0 0");
        CHECK(line != "and_then 1 0");
        CHECK(line != "and_then_val 0");
        CHECK(line != "or_else 0 1 0");
        CHECK(line != "or_else_unex 1 0");
        CHECK(line != "and_then_unex 0 0 0");
        CHECK(line != "or_else_e 1 0");
        CHECK(line != "and_then_r 0 0");
        CHECK(line.find("callT") == std::string::npos);
        CHECK(line.find("callR") == std::string::npos);
        CHECK(line.find("callEo") == std::string::npos);
        CHECK(line.find("callRu") == std::string::npos);
        CHECK(line.find("callRv") == std::string::npos);
        CHECK(line.find("callOr") == std::string::npos);
        CHECK(line.find("callCmv") == std::string::npos);
        CHECK(line.find("callCOr") == std::string::npos);
        CHECK(line != "value_or 99");
        CHECK(line != "value_or 0");
        CHECK(line != "value_or_unex 0");
        CHECK(line != "value_or_unex 9");
        CHECK(line != "value_or_rv 99");
        CHECK(line != "value_or_rv 0");
        CHECK(line != "value_or_rv_unex 0");
        CHECK(line != "value_or_rv_unex 7");
        CHECK(line != "value_or_zero 99");
        CHECK(line != "value_or_const 99");
        CHECK(line != "value_or_const 0");
        CHECK(line != "value_or_const_unex 0");
        CHECK(line != "value_or_const_unex 3");
        CHECK(line != "value_or_held 99");
        CHECK(line != "value_or_held 0");
        CHECK(line != "value_or_none 0");
        CHECK(line != "value_or_none 4");
        CHECK(line != "transform 1 0");
        CHECK(line != "transform 1 42");
        CHECK(line != "transform_unex 0 0 0");
        CHECK(line != "transform_unex 0 1 9");
        CHECK(line != "transform_unex 1 0 9");
        CHECK(line != "transform_src 0");
        CHECK(line != "transform_zero 1 99");
        CHECK(line != "transform_zero 0 0");
        CHECK(line != "transform_err0 1 0");
        CHECK(line != "transform_err0 1 99");
        CHECK(line != "transform_err0 0 99");
        CHECK(line != "transform_const 42");
        CHECK(line != "transform_const 0");
        CHECK(line != "transform_const_unex 0 0 0");
        CHECK(line != "transform_const_unex 0 1 3");
        CHECK(line != "transform_rv 11");
        CHECK(line != "transform_rv 0");
        CHECK(line != "transform_rv_unex 1 6");
        CHECK(line != "transform_rv_unex 0 0");
        CHECK(line != "transform_cmv_val 1 8");
        CHECK(line != "transform_cmv_val 1 0");
        CHECK(line != "transform_cmv 1 4");
        CHECK(line != "transform_cmv 0 0");
        CHECK(line != "transform_not 1 0");
        CHECK(line != "transform_not 0 0");
        CHECK(line != "transform_val 0");
        CHECK(line != "transform_val 0 6");
        CHECK(line != "transform_box 0");
        CHECK(line != "transform_box 99");
        CHECK(line != "transform_box_unex 0 0 0");
        CHECK(line != "transform_box_unex 0 1 9");
        CHECK(line != "transform_box_unex 1 0 9");
        CHECK(line != "transform_r 0 0");
        CHECK(line != "transform_r 1 3");
        CHECK(line.find("skipEt") == std::string::npos);
        CHECK(line.find("skipRv") == std::string::npos);
        CHECK(line.find("skipCmv") == std::string::npos);
        CHECK(line.find("skipZ") == std::string::npos);
        CHECK(line.find("skipEr") == std::string::npos);
        CHECK(line != "transform_error 0 0");
        CHECK(line != "transform_error 0 9");
        CHECK(line != "transform_error 1 18");
        CHECK(line != "transform_error 1 0");
        CHECK(line != "transform_error_src_err 0");
        CHECK(line != "transform_error_src_err 18");
        CHECK(line != "transform_error_val 1 1 42");
        CHECK(line != "transform_error_val 0 1 0");
        CHECK(line != "transform_error_val 0 0 42");
        CHECK(line != "transform_error_src 0");
        CHECK(line != "transform_error_src 9");
        CHECK(line != "transform_error_zero 1 99");
        CHECK(line != "transform_error_zero 0 0");
        CHECK(line != "transform_error_zero 0 99");
        CHECK(line != "transform_error_err0 1 0");
        CHECK(line != "transform_error_err0 0 0");
        CHECK(line != "transform_error_err0 1 99");
        CHECK(line != "transform_error_zero_err 1 0");
        CHECK(line != "transform_error_zero_err 0 5");
        CHECK(line != "transform_error_zero_err 1 5");
        CHECK(line != "transform_error_const_val 0");
        CHECK(line != "transform_error_const_val 3");
        CHECK(line != "transform_error_const 1 10");
        CHECK(line != "transform_error_const 0 3");
        CHECK(line != "transform_error_const 0 0");
        CHECK(line != "transform_error_rv_val 0");
        CHECK(line != "transform_error_rv_val 6");
        CHECK(line != "transform_error_rv 1 7");
        CHECK(line != "transform_error_rv 0 6");
        CHECK(line != "transform_error_rv 0 0");
        CHECK(line != "transform_error_cmv_val 1 0");
        CHECK(line != "transform_error_cmv_val 0 8");
        CHECK(line != "transform_error_cmv 1 5");
        CHECK(line != "transform_error_cmv 0 4");
        CHECK(line != "transform_error_cmv 0 0");
        CHECK(line != "transform_error_not 1 9");
        CHECK(line != "transform_error_not 0 4");
        CHECK(line != "transform_error_not 0 0");
        CHECK(line != "transform_error_held 1 0");
        CHECK(line != "transform_error_held 0 6");
        CHECK(line != "transform_error_held 1 7");
        CHECK(line != "transform_error_box 0");
        CHECK(line != "transform_error_box 42");
        CHECK(line != "transform_error_box_val 1 42");
        CHECK(line != "transform_error_box_val 0 0");
        CHECK(line != "transform_error_box_val 0 9");
        CHECK(line != "terr 0");
        CHECK(line != "transform_error_r 1 4");
        CHECK(line != "transform_error_r 0 3");
        CHECK(line != "transform_error_r 0 0");
        CHECK(line != "transform_error_kept 0 42");
        CHECK(line != "transform_error_kept 1 0");
        CHECK(line != "transform_error_cat 1");
        CHECK(line != "transform_error_cat 21");
        CHECK(line != "transform_error_cat 31");
        CHECK(line != "transform_error_cat 41");
        CHECK(line != "transform_error_cat_const 1");
        CHECK(line != "transform_error_cat_const 11");
        CHECK(line != "transform_error_cat_const 31");
        CHECK(line != "transform_error_cat_const 41");
        CHECK(line != "transform_error_cat_rv 1");
        CHECK(line != "transform_error_cat_rv 11");
        CHECK(line != "transform_error_cat_rv 21");
        CHECK(line != "transform_error_cat_rv 41");
        CHECK(line != "transform_error_cat_cmv 1");
        CHECK(line != "transform_error_cat_cmv 11");
        CHECK(line != "transform_error_cat_cmv 21");
        CHECK(line != "transform_error_cat_cmv 31");
        CHECK(line.find("skipZt") == std::string::npos);
        CHECK(line.find("skipTeC") == std::string::npos);
        CHECK(line.find("skipRvT") == std::string::npos);
        CHECK(line.find("skipCmvT") == std::string::npos);
        CHECK(line.find("skipBox") == std::string::npos);
        CHECK(line.find("callTe") == std::string::npos);
    }
    CHECK(saw_stored);
    CHECK(saw_error);
    CHECK(saw_and_then);
    CHECK(saw_and_then_unex);
    CHECK(saw_or_else);
    CHECK(saw_or_else_unex);
    CHECK(saw_and_then_not);
    CHECK(saw_or_else_not);
    CHECK(saw_got);
    CHECK(saw_and_then_val);
    CHECK(saw_or_else_e);
    CHECK(saw_and_then_r);
    CHECK(saw_and_then_const);
    CHECK(saw_or_else_const);
    CHECK(saw_and_then_rv);
    CHECK(saw_or_else_rv);
    CHECK(saw_and_then_rv_unex);
    CHECK(saw_or_else_rv_val);
    CHECK(saw_and_then_cmv);
    CHECK(saw_or_else_cmv);
    CHECK(saw_value_or);
    CHECK(saw_value_or_unex);
    CHECK(saw_value_or_rv);
    CHECK(saw_value_or_rv_unex);
    CHECK(saw_value_or_zero);
    CHECK(saw_value_or_const);
    CHECK(saw_value_or_const_unex);
    CHECK(saw_value_or_held);
    CHECK(saw_value_or_none);
    CHECK(saw_transform);
    CHECK(saw_transform_unex);
    CHECK(saw_transform_src);
    CHECK(saw_transform_zero);
    CHECK(saw_transform_err0);
    CHECK(saw_transform_const);
    CHECK(saw_transform_const_unex);
    CHECK(saw_transform_rv);
    CHECK(saw_transform_rv_unex);
    CHECK(saw_transform_cmv_val);
    CHECK(saw_transform_cmv);
    CHECK(saw_transform_not);
    CHECK(saw_xform);
    CHECK(saw_transform_val);
    CHECK(saw_transform_box);
    CHECK(saw_transform_box_unex);
    CHECK(saw_transform_r);
    CHECK(saw_transform_error);
    CHECK(saw_transform_error_src_err);
    CHECK(saw_transform_error_val);
    CHECK(saw_transform_error_src);
    CHECK(saw_transform_error_zero);
    CHECK(saw_transform_error_err0);
    CHECK(saw_transform_error_zero_err);
    CHECK(saw_transform_error_const_val);
    CHECK(saw_transform_error_const);
    CHECK(saw_transform_error_rv_val);
    CHECK(saw_transform_error_rv);
    CHECK(saw_transform_error_cmv_val);
    CHECK(saw_transform_error_cmv);
    CHECK(saw_transform_error_not);
    CHECK(saw_transform_error_held);
    CHECK(saw_transform_error_box);
    CHECK(saw_transform_error_box_val);
    CHECK(saw_terr);
    CHECK(saw_transform_error_r);
    CHECK(saw_transform_error_kept);
    CHECK(saw_transform_error_cat);
    CHECK(saw_transform_error_cat_const);
    CHECK(saw_transform_error_cat_rv);
    CHECK(saw_transform_error_cat_cmv);

    CHECK(failed_is_check(mod, "value", "CXX-EXPECTED-NULL", "expected::value()"));
    CHECK(failed_is_check(mod, "star", "CXX-EXPECTED-NULL", "expected::operator*"));
    CHECK(failed_is_check(mod, "error", "CXX-EXPECTED-ERROR", "expected::error()"));
}

TEST_CASE("cxx expected model: equality, less, and emplace bodies") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "expected";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#include <new>") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <compare>") == std::string::npos);
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 5);

    const auto class_open = src.find("class expected {");
    const auto em = src.find("_Tp& emplace(_Up&& __u)");
    const auto eq_friend = src.find("friend constexpr bool operator==(const expected<_Up, _Ep>&");
    const auto lt_friend = src.find("friend constexpr bool operator<(const expected<_Up, _Ep>&");
    const auto class_close = src.find("};", lt_friend);
    const auto eq = src.find("constexpr bool operator==(const expected<_Tp, _Er>&");
    const auto ne = src.find("constexpr bool operator!=(const expected<_Tp, _Er>&");
    const auto lt = src.find("constexpr bool operator<(const expected<_Tp, _Er>&");
    REQUIRE(class_open != std::string::npos);
    REQUIRE(em != std::string::npos);
    REQUIRE(eq_friend != std::string::npos);
    REQUIRE(lt_friend != std::string::npos);
    REQUIRE(class_close != std::string::npos);
    REQUIRE(eq != std::string::npos);
    REQUIRE(ne != std::string::npos);
    REQUIRE(lt != std::string::npos);
    CHECK(class_open < em);
    CHECK(em < eq_friend);
    CHECK(eq_friend < lt_friend);
    CHECK(lt_friend < class_close);
    CHECK(class_close < eq);
    CHECK(eq < ne);
    CHECK(ne < lt);
    CHECK(src.find("friend constexpr bool operator!=") == std::string::npos);

    const std::string em_body = function_body(src, em);
    const std::string eq_body = function_body(src, eq);
    const std::string ne_body = function_body(src, ne);
    const std::string lt_body = function_body(src, lt);
    REQUIRE_FALSE(em_body.empty());
    REQUIRE_FALSE(eq_body.empty());
    REQUIRE_FALSE(ne_body.empty());
    REQUIRE_FALSE(lt_body.empty());

    const auto em_if = em_body.find("if (_M_has)");
    const auto em_else = em_body.find("} else {");
    const auto em_new =
        em_body.find("::new (static_cast<void*>(&_M_storage._M_val)) _Tp(static_cast<_Up&&>(__u));");
    const auto em_ret = em_body.find("return _M_storage._M_val;");
    REQUIRE(em_if != std::string::npos);
    REQUIRE(em_else != std::string::npos);
    REQUIRE(em_new != std::string::npos);
    REQUIRE(em_ret != std::string::npos);
    CHECK(em_if < em_else);
    CHECK(em_else < em_new);
    CHECK(em_new < em_ret);
    CHECK(em_body.find("_M_val.~_Tp()") != std::string::npos);
    CHECK(em_body.find("_M_unex.~_Er()") != std::string::npos);
    CHECK(em_body.find("_M_has = true;") != std::string::npos);
    CHECK(em_body.substr(em_if, em_else - em_if).find("_M_unex") == std::string::npos);
    CHECK(em_body.substr(em_else, em_new - em_else).find("_M_val") == std::string::npos);

    const auto eq_false = eq_body.find("return false;");
    REQUIRE(eq_false != std::string::npos);
    CHECK(eq_body.substr(0, eq_false).find("_M_storage") == std::string::npos);
    CHECK(eq_body.substr(0, eq_false).find("_M_val") == std::string::npos);
    CHECK(eq_body.substr(0, eq_false).find("_M_unex") == std::string::npos);
    CHECK(eq_body.find("if (__x._M_has != __y._M_has)") != std::string::npos);
    CHECK(eq_body.find("return __x._M_storage._M_val == __y._M_storage._M_val;") != std::string::npos);
    CHECK(eq_body.find("return __x._M_storage._M_unex == __y._M_storage._M_unex;") != std::string::npos);
    const auto eq_val = eq_body.find("_M_val");
    const auto eq_unex = eq_body.find("_M_unex");
    REQUIRE(eq_val != std::string::npos);
    REQUIRE(eq_unex != std::string::npos);
    CHECK(eq_val < eq_unex);
    CHECK(eq_body.substr(eq_val, eq_unex - eq_val).find("_M_unex") == std::string::npos);
    CHECK(eq_body.find("_M_val", eq_unex) == std::string::npos);

    CHECK(ne_body.find("return !(__x == __y);") != std::string::npos);
    CHECK(ne_body.find("_M_storage") == std::string::npos);
    CHECK(ne_body.find("_M_val") == std::string::npos);
    CHECK(ne_body.find("_M_unex") == std::string::npos);
    CHECK(ne_body.find("_M_has") == std::string::npos);
    CHECK(ne_body.find("<") == std::string::npos);

    const auto lt_storage = lt_body.find("_M_storage");
    REQUIRE(lt_storage != std::string::npos);
    CHECK(lt_body.find("if (__x._M_has && !__y._M_has)") != std::string::npos);
    CHECK(lt_body.find("return false;") != std::string::npos);
    CHECK(lt_body.find("if (!__x._M_has && __y._M_has)") != std::string::npos);
    CHECK(lt_body.find("return true;") != std::string::npos);
    CHECK(prism_test::first_before(lt_body, "return false;", lt_storage));
    CHECK(prism_test::first_before(lt_body, "return true;", lt_storage));
    CHECK(lt_body.substr(0, lt_storage).find("_M_val") == std::string::npos);
    CHECK(lt_body.substr(0, lt_storage).find("_M_unex") == std::string::npos);
    CHECK(lt_body.find("return __x._M_storage._M_val < __y._M_storage._M_val;") != std::string::npos);
    CHECK(lt_body.find("return __x._M_storage._M_unex < __y._M_storage._M_unex;") != std::string::npos);
    const auto lt_val = lt_body.find("_M_val");
    const auto lt_unex = lt_body.find("_M_unex");
    REQUIRE(lt_val != std::string::npos);
    REQUIRE(lt_unex != std::string::npos);
    CHECK(lt_val < lt_unex);
    CHECK(lt_body.substr(lt_val, lt_unex - lt_val).find("_M_unex") == std::string::npos);
    CHECK(lt_body.find("_M_val", lt_unex) == std::string::npos);
    CHECK(lt_body.find("operator==") == std::string::npos);
    CHECK(lt_body.find("==") == std::string::npos);

    for (const std::string& body : {em_body, eq_body, ne_body, lt_body}) {
        CHECK(body.find("value()") == std::string::npos);
        CHECK(body.find("error()") == std::string::npos);
        CHECK(body.find("operator*") == std::string::npos);
        CHECK(body.find("operator->") == std::string::npos);
        CHECK(body.find("__prism_check") == std::string::npos);
        CHECK(body.find("_Tp{}") == std::string::npos);
        CHECK(body.find("_Er{}") == std::string::npos);
        CHECK(body.find("T{}") == std::string::npos);
        CHECK(body.find("E{}") == std::string::npos);
    }

    const std::string code = strip_comments(src);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
    CHECK(code.find("swap") == std::string::npos);
    CHECK(code.find("expected<void") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("operator>=") == std::string::npos);
    CHECK(code.find("operator<=") == std::string::npos);
    CHECK(code.find("operator>") == std::string::npos);
    CHECK(code.find("initializer_list") == std::string::npos);
    CHECK(count_of(code, "emplace(") == 1);
    CHECK(count_of(code, "operator==") == 2);
    CHECK(count_of(code, "operator!=") == 1);
    CHECK(count_of(code, "operator<") == 2);
    CHECK(count_of(code, "CXX-EXPECTED-NULL") == 6);
    CHECK(count_of(code, "CXX-EXPECTED-ERROR") == 2);
    CHECK(code.find("and_then") != std::string::npos);
    CHECK(code.find("or_else") != std::string::npos);
    CHECK(code.find("value_or") != std::string::npos);
    CHECK(code.find("transform_error") != std::string::npos);
    CHECK(code.find("#include <new>") != std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
}

TEST_CASE("cxx expected model: operator< is same-type and is not in libstdc++") {
    auto& c = ctx();
    if (c.cxx.empty()) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = c.tmp / "expected_less.cpp";
    std::ofstream(src) << R"EOS(#include <cstdio>
#include <cstdlib>
#include <expected>
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) { std::fprintf(stderr, "CHECK %s: %s\n", cls, msg); std::abort(); }
}
struct LtVal {
    int v;
    LtVal() = delete;
    explicit LtVal(int x) : v(x) {}
    friend bool operator<(const LtVal& a, const LtVal& b) {
        std::printf("ltv %d %d\n", a.v, b.v);
        return a.v < b.v;
    }
};
struct LtErr {
    int v;
    LtErr() = delete;
    explicit LtErr(int x) : v(x) {}
    friend bool operator<(const LtErr& a, const LtErr& b) {
        std::printf("lte %d %d\n", a.v, b.v);
        return a.v < b.v;
    }
};
int main() {
    std::expected<int, int> a(1), b(2), z(0), z2(0);
    std::printf("lt_val %d %d %d %d\n", static_cast<int>(a < b), static_cast<int>(b < a),
                static_cast<int>(a < a), static_cast<int>(z < z2));
    std::expected<int, int> e1(std::unexpected<int>(1));
    std::expected<int, int> e2(std::unexpected<int>(2));
    std::expected<int, int> ez(std::unexpected<int>(0));
    std::expected<int, int> ez2(std::unexpected<int>(0));
    std::printf("lt_err %d %d %d %d\n", static_cast<int>(e1 < e2), static_cast<int>(e2 < e1),
                static_cast<int>(e1 < e1), static_cast<int>(ez < ez2));
    std::expected<int, int> v0(0), v9(9);
    std::expected<int, int> err9(std::unexpected<int>(9));
    std::expected<int, int> err0(std::unexpected<int>(0));
    std::printf("lt_mix %d %d %d %d %d %d\n", static_cast<int>(v0 < err9), static_cast<int>(err9 < v0),
                static_cast<int>(v9 < err0), static_cast<int>(err0 < v9), static_cast<int>(v0 < err0),
                static_cast<int>(err0 < v0));
    const std::expected<int, int> ca(1), cb(2), ce(std::unexpected<int>(3));
    std::printf("lt_const %d %d %d %d\n", static_cast<int>(ca < cb), static_cast<int>(cb < ca),
                static_cast<int>(ce < ca), static_cast<int>(ca < ce));
    std::expected<LtVal, LtErr> va(LtVal(3)), vb(LtVal(3)), vc(LtVal(4));
    const int ab = static_cast<int>(va < vb);
    const int ac = static_cast<int>(va < vc);
    const int cva = static_cast<int>(vc < va);
    std::printf("lt_t %d %d %d\n", ab, ac, cva);
    std::expected<LtVal, LtErr> ea(std::unexpected<LtErr>(LtErr(5)));
    std::expected<LtVal, LtErr> eb(std::unexpected<LtErr>(LtErr(5)));
    std::expected<LtVal, LtErr> ec(std::unexpected<LtErr>(LtErr(6)));
    const int es = static_cast<int>(ea < eb);
    const int ed = static_cast<int>(ea < ec);
    const int eg = static_cast<int>(ec < ea);
    std::printf("lt_e %d %d %d\n", es, ed, eg);
    std::expected<LtVal, LtErr> mix_v(LtVal(8));
    std::expected<LtVal, LtErr> mix_e(std::unexpected<LtErr>(LtErr(2)));
    const int ve = static_cast<int>(mix_v < mix_e);
    const int ev = static_cast<int>(mix_e < mix_v);
    std::printf("lt_cross %d %d\n", ve, ev);
    return 0;
}
)EOS";
    if (c.ok) CHECK_FALSE(syntax_only(c.cxx, src, false));
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back(src.string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "expected").string())) != std::string::npos);

    auto mod = run_lines(build_src(c.cxx, c.tmp, src, true, "expected_less"));
    const std::vector<std::string> want{
        "lt_val 1 0 0 0", "lt_err 1 0 0 0", "lt_mix 0 1 0 1 0 1", "lt_const 1 0 1 0", "ltv 3 3",
        "ltv 3 4",        "ltv 4 3",        "lt_t 0 1 0",         "lte 5 5",         "lte 5 6",
        "lte 6 5",        "lt_e 0 1 0",     "lt_cross 0 1",
    };
    CHECK(mod == want);
    for (const auto& line : mod) {
        CHECK(line.find("ltv 8") == std::string::npos);
        CHECK(line.find("lte 2") == std::string::npos);
    }
}

TEST_CASE("cxx expected model: omitted comparisons and emplace arities stay undeclared") {
    auto& c = ctx();
    if (c.cxx.empty()) return prism_test::notrun(cxxh::kNoCompiler);
    struct Snip {
        const char* name;
        const char* src;
        bool model_ok;
        bool lib_ok;
    };
    const Snip snips[] = {
        {"same_eq",
         "#include <expected>\n"
         "struct NoDef {\n"
         "  int v;\n"
         "  explicit NoDef(int x) : v(x) {}\n"
         "  bool operator==(const NoDef& o) const { return v == o.v; }\n"
         "};\n"
         "int use(){\n"
         "  std::expected<NoDef, int> a(NoDef(1));\n"
         "  std::expected<NoDef, int> b(NoDef(2));\n"
         "  std::expected<NoDef, int> e(std::unexpected<int>(3));\n"
         "  return ((a == b) || (a != b)) && !(a == e) && (a != e) ? 1 : 0;\n"
         "}\n",
         true, true},
        {"store",
         "#include <expected>\n"
         "struct NoEq { int v; explicit NoEq(int x) : v(x) {} };\n"
         "int use(){ std::expected<NoEq, int> a(NoEq(1)); return a.has_value() ? 1 : 0; }\n",
         true, true},
        {"no_eq",
         "#include <expected>\n"
         "struct NoEq { int v; explicit NoEq(int x) : v(x) {} };\n"
         "int use(){ std::expected<NoEq, int> a(NoEq(1)); std::expected<NoEq, int> b(NoEq(2));\n"
         "  return a == b ? 1 : 0; }\n",
         false, false},
        {"diff_type",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1); std::expected<long, long> b(1L);\n"
         "  return a == b ? 1 : 0; }\n",
         false, true},
        {"bare",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1); return a == 1 ? 1 : 0; }\n",
         false, true},
        {"unex",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(std::unexpected<int>(1));\n"
         "  return a == std::unexpected<int>(1) ? 1 : 0; }\n",
         false, true},
        {"emplace_arity",
         "#include <expected>\n"
         "struct Two { int a; int b; Two(int x, int y) noexcept : a(x), b(y) {} };\n"
         "int use(){ std::expected<int, int> z(std::unexpected<int>(1)); z.emplace();\n"
         "  std::expected<Two, int> t(std::unexpected<int>(1)); t.emplace(1, 2); return 0; }\n",
         false, true},
        {"swap",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1), b(2); a.swap(b); return a.value(); }\n",
         false, true},
        {"same_lt",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1), b(2);\n"
         "  std::expected<int, int> e(std::unexpected<int>(3));\n"
         "  return (a < b) && !(b < a) && !(a < a) && (e < a) && !(a < e) ? 1 : 0; }\n",
         true, false},
        {"diff_lt",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1); std::expected<long, long> b(2L);\n"
         "  return a < b ? 1 : 0; }\n",
         false, false},
        {"no_lt",
         "#include <expected>\n"
         "struct NoLt { int v; explicit NoLt(int x) : v(x) {} };\n"
         "int use(){ std::expected<NoLt, int> a(NoLt(1)); std::expected<NoLt, int> b(NoLt(2));\n"
         "  return a < b ? 1 : 0; }\n",
         false, false},
        {"gt",
         "#include <expected>\n"
         "int use(){ std::expected<int, int> a(1), b(2); return a > b ? 1 : 0; }\n",
         false, false},
    };
    for (const auto& snip : snips) {
        CAPTURE(snip.name);
        const fs::path file = c.tmp / (std::string("expected_") + snip.name + ".cpp");
        std::ofstream(file) << snip.src;
        std::string err;
        CHECK(syntax_only(c.cxx, file, true, &err) == snip.model_ok);
        if (c.ok) CHECK(syntax_only(c.cxx, file, false, &err) == snip.lib_ok);
    }
}
