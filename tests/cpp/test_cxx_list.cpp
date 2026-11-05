// Differential test of the std::list model header (src/prism/pir/models/cxx/list).
// Same shape as tests/cpp/test_cxx_models.cpp; that file is not edited, so this
// driver is its own translation unit. CMake's prism_tests source list is explicit
// (not a glob): docs/finish-notes/list.md has the line that links this file.

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

struct ListCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

ListCtx& ctx() {
    static ListCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <list>\n"
                                  "int main() { std::list<int> v{1}; return v.front() - 1; }\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("list_trace.cpp"), "list_trace", cxxh::asan_flags(), with_models);
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

}  // namespace

TEST_CASE("cxx list model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxlist-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx list model: trace matches libstdc++ and erase is use-after-free") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "list_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "list").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 20);
    CHECK(a == b);
#ifndef _WIN32
    for (const fs::path& exe : {lib, mod}) {
        std::string out;
        int rc = run_capture(exe, "uaf", &out);
        CHECK(rc != 0);
        CHECK(out.find("heap-use-after-free") != std::string::npos);
    }
#endif
}
