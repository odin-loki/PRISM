// Differential tests of PRISM C++ library model headers (roadmap 2.6).
// Port of tests/test_cxx_models.py; data stays in tests/cxx_models/.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

struct CxxModelsCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

CxxModelsCtx& ctx() {
    static CxxModelsCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    // <bits/c++config.h> is libstdc++'s: the model headers replace its containers and
    // include its internals, so a clang on another standard library (the MSVC STL) has
    // nothing to compare against and the cases are NOTRUN, not failures.
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <vector>\n"
                                  "int main() { std::vector<int> v{1}; return v[0] - 1; }\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, const std::string& prog, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src(prog + ".cpp"), prog, cxxh::asan_flags(), with_models);
}

std::string headers(const std::string& cxx, const std::string& prog) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back("-M");
    argv.push_back((repo() / "tests" / "cxx_models" / (prog + ".cpp")).string());
    std::string out;
    REQUIRE(run_cmd(argv, &out));
    return out;
}

// min_lines: the trace program's line count (the long traces print > 20).
void same_trace(const std::string& prog, const std::vector<const char*>& model_headers, std::size_t min_lines = 21) {
    auto& c = ctx();
    REQUIRE(c.ok);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    auto deps = headers(c.cxx, prog);
    deps = fwd(deps);
    for (const char* m : model_headers) CHECK(deps.find(fwd((models / m).string())) != std::string::npos);
    auto lib = build_prog(c.cxx, c.tmp, prog, false);
    auto mod = build_prog(c.cxx, c.tmp, prog, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() >= min_lines);
    CHECK(a == b);
}

}  // namespace

TEST_CASE("cxx models: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxmodels-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan not usable here");
}

TEST_CASE("cxx models: vector trace matches libstdc++") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("vector_trace", {"vector"});
}

TEST_CASE("cxx models: string trace matches libstdc++") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("string_trace", {"bits/basic_string.tcc"});
}

TEST_CASE("cxx models: sstream trace matches libstdc++") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("sstream_trace", {"sstream"});
}

#ifndef _WIN32
int run_capture(const fs::path& exe, const std::string& arg, std::string* combined) {
    std::string cmd = exe.string() + " " + arg + " 2>&1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    combined->clear();
    char buf[4096];
    while (fgets(buf, sizeof buf, fp)) *combined += buf;
    return pclose(fp);
}
#endif

TEST_CASE("cxx models: unordered_map/set trace matches libstdc++ and UAF on erase") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("unordered_map_set_trace", {"unordered_map", "unordered_set", "prism_hash_table.h"});
#ifndef _WIN32
    auto& c = ctx();
    for (bool models : {false, true}) {
        auto exe = c.tmp / ("unordered_map_set_trace" + std::string(models ? ".model" : ".lib"));
        std::string out;
        int rc = run_capture(exe, "uaf", &out);
        CHECK(rc != 0);
        CHECK(out.find("heap-use-after-free") != std::string::npos);
    }
#endif
}

TEST_CASE("cxx models: map/set trace matches libstdc++ and UAF on erase") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("map_set_trace", {"map", "set", "prism_tree.h"});
#ifndef _WIN32
    auto& c = ctx();
    for (bool models : {false, true}) {
        auto exe = c.tmp / ("map_set_trace" + std::string(models ? ".model" : ".lib"));
        std::string out;
        int rc = run_capture(exe, "uaf", &out);
        CHECK(rc != 0);
        CHECK(out.find("heap-use-after-free") != std::string::npos);
    }
#endif
}

TEST_CASE("cxx models: ranges views::take/drop trace matches libstdc++") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("ranges_views_trace", {"ranges"}, 4);  // n = 0, 2, 5, 9
}

TEST_CASE("cxx models: variant visit trace matches libstdc++") {
    if (!ctx().ok) return prism_test::notrun(cxxh::kNoCompiler);
    same_trace("variant_visit_trace", {"variant"}, 4);
}
