// Differential test of the integer std::from_chars / std::to_chars model
// (src/prism/pir/models/cxx/charconv). Same shape as tests/cpp/test_cxx_list.cpp:
// this driver is its own translation unit. CMake's prism_tests source list is
// explicit (not a glob). The trace is tests/cxx_models/charconv_trace.cpp.
//
// Without a libstdc++ clang++ that can run ASan/UBSan the cases report NOTRUN.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <algorithm>
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

struct CharconvCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
    bool built = false;
    fs::path model_exe;
    std::vector<std::string> lib_lines;
    std::vector<std::string> model_lines;
};

CharconvCtx& ctx() {
    static CharconvCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <charconv>\n"
                                  "int main() {\n"
                                  "  int v = 0;\n"
                                  "  const char* s = \"7\";\n"
                                  "  auto r = std::from_chars(s, s + 1, v);\n"
                                  "  return v - 7 + (r.ec == std::errc{} ? 0 : 1);\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("charconv_trace.cpp"), "charconv_trace", cxxh::asan_flags(), with_models);
}

bool has_line(const std::vector<std::string>& lines, const char* exact) {
    return std::find(lines.begin(), lines.end(), exact) != lines.end();
}

void ensure_built() {
    auto& c = ctx();
    if (!c.ok || c.built) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "charconv_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    REQUIRE(fwd(deps).find(fwd((models / "charconv").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    c.model_exe = build_prog(c.cxx, c.tmp, true);
    c.lib_lines = run_lines(lib);
    c.model_lines = run_lines(c.model_exe);
    c.built = true;
}

// A failing __prism_check aborts. The stand-in prints CHECK <class>: <message>
// and must not reach the "fake success" printf.
void failed_is_check(const fs::path& exe, const char* arg, const char* cls, const char* which) {
    std::string out;
    std::string err;
    CHECK_FALSE(run_cmd({exe.string(), arg}, &out, &err));
    CHECK(out.find("fake=") == std::string::npos);
    const std::string needle = std::string("CHECK ") + cls + ":";
    CHECK(err.find(needle) != std::string::npos);
    CHECK(err.find(which) != std::string::npos);
}

}  // namespace

TEST_CASE("cxx charconv model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxcharconv-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx charconv model: a small number stores that number") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    ensure_built();
    CHECK(c.lib_lines == c.model_lines);
    CHECK(has_line(c.model_lines, "small ec=1 v=42 off=2"));
}

TEST_CASE("cxx charconv model: overflow does not store a success value") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    ensure_built();
    CHECK(c.lib_lines == c.model_lines);
    // Sentinels stay put: 12345 is not a wrapped int, 77 is not a zero from
    // 2^32, and ec=2 is result_out_of_range rather than success.
    CHECK(has_line(c.model_lines, "over ec=2 v=12345 off=10"));
    CHECK(has_line(c.model_lines, "over0 ec=2 v=77 off=10"));
    CHECK(has_line(c.model_lines, "uover ec=2 v=5 off=10"));
    CHECK(has_line(c.model_lines, "minover ec=2 v=3 off=11"));
    // A buffer of two bytes cannot hold 100000. ec=4 is value_too_large,
    // success=0, and the canary bytes are unchanged (nothing written past last).
    CHECK(has_line(c.model_lines, "short ec=4 success=0 b0=Q b1=Q z2=Z z3=Z"));
}

TEST_CASE("cxx charconv model: a null range is a check") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    ensure_built();
    failed_is_check(c.model_exe, "null", "PTR-NULL-DEREF", "from_chars");
    failed_is_check(c.model_exe, "invert", "FUNC-CONTRACT", "last before first");
}
