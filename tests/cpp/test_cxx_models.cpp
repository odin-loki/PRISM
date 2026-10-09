// Differential tests of PRISM C++ library model headers (ports tests/test_cxx_models.py).
// POSIX + clang++ with ASan/UBSan only.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#ifndef _WIN32

#include "../../src/prism/proc.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

struct Tmp {
    fs::path dir;
    Tmp() {
        static int n = 0;
        dir = fs::temp_directory_path() / ("prism-cxxmodels-" + std::to_string(++n));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

prism::detail::RunOut run_env(const std::vector<std::string>& argv, const fs::path& cwd) {
    prism::detail::RunSpec spec;
    spec.argv = argv;
    spec.cwd = cwd;
    spec.env = {{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1"}, {"UBSAN_OPTIONS", "halt_on_error=1"}};
    spec.merge_stderr = true;
    return prism::detail::run(spec);
}

std::string which_cxx() {
    for (const char* c : {"clang++-18", "clang++"}) {
        auto r = prism::detail::run_process({"which", c}, 30, repo());
        if (r.rc == 0 && !r.text.empty()) {
            std::string p = r.text;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            return p;
        }
    }
    return {};
}

bool sanitizer_ok(const std::string& cxx, const fs::path& tmp) {
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <vector>\nint main() { std::vector<int> v{1}; return v[0] - 1; }\n";
    auto out = tmp / "probe";
    const std::vector<std::string> flags = {"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                            "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build = {cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.push_back("-o");
    build.push_back(out.string());
    if (prism::detail::run_process(build, 600, tmp).rc != 0) return false;
    return run_env({out.string()}, tmp).rc == 0;
}

std::string run_capture(const std::vector<std::string>& argv, const fs::path& cwd) {
    return run_env(argv, cwd).out;
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, const std::string& prog, bool models) {
    auto out = tmp / (prog + (models ? ".model" : ".lib"));
    std::vector<std::string> argv = {cxx, "-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                     "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    if (models) {
        argv.push_back("-isystem");
        argv.push_back((repo() / "src" / "prism" / "pir" / "models" / "cxx").string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / (prog + ".cpp")).string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(prism::detail::run_process(argv, 600, tmp).rc == 0);
    return out;
}

void same_trace(const std::string& cxx, Tmp& tmp, const std::string& prog, const std::vector<std::string>& model_hdrs) {
    auto deps_argv = std::vector<std::string>{cxx, "-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                              "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-isystem",
                                              (repo() / "src" / "prism" / "pir" / "models" / "cxx").string(),
                                              "-M",
                                              (repo() / "tests" / "cxx_models" / (prog + ".cpp")).string()};
    auto dep_out = prism::detail::run_process(deps_argv, 300, tmp.dir).text;
    for (const auto& h : model_hdrs) {
        auto want = (repo() / "src" / "prism" / "pir" / "models" / "cxx" / h).string();
        CHECK(dep_out.find(want) != std::string::npos);
    }
    auto lib = build_prog(cxx, tmp.dir, prog, false);
    auto mod = build_prog(cxx, tmp.dir, prog, true);
    auto a = run_capture({lib.string()}, tmp.dir);
    auto b = run_capture({mod.string()}, tmp.dir);
    std::vector<std::string> la, lb;
    std::istringstream ia(a), ib(b);
    std::string line;
    while (std::getline(ia, line)) la.push_back(line);
    while (std::getline(ib, line)) lb.push_back(line);
    CHECK(la.size() > 20);
    CHECK(la == lb);
}

}  // namespace

TEST_CASE("cxx_models: vector/string/map differential traces") {
    const auto cxx = which_cxx();
    if (cxx.empty()) {
        MESSAGE("NOTRUN: clang++ not found");
        return;
    }
    Tmp tmp;
    if (!sanitizer_ok(cxx, tmp.dir)) {
        MESSAGE("NOTRUN: clang++ ASan/UBSan runtimes not usable");
        return;
    }
    same_trace(cxx, tmp, "vector_trace", {"vector"});
    same_trace(cxx, tmp, "string_trace", {"bits/basic_string.tcc"});
    same_trace(cxx, tmp, "map_set_trace", {"map", "set", "prism_tree.h"});
    for (bool models : {false, true}) {
        auto exe = build_prog(cxx, tmp.dir, "map_set_trace", models);
        auto r = run_env({exe.string(), "uaf"}, tmp.dir);
        CHECK(r.rc != 0);
        CHECK(r.out.find("heap-use-after-free") != std::string::npos);
    }
}

#endif
