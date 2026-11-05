// Differential test of the std::deque model header. Standalone: it does not
// share fixtures with tests/cpp/test_cxx_models.cpp. The header is on the
// pir include path (embedded as cxx/deque); this test compiles
// tests/cxx_models/deque_trace.cpp twice, once against libstdc++ and once with
// -isystem on a directory holding only the model, so #include <deque> is it.
//
// Without a libstdc++ clang++ that can run ASan/UBSan the cases report NOTRUN.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
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

// A directory holding only the model <deque> (src/prism/pir/models/cxx/deque),
// so -isystem swaps that one header and leaves the other models out.
fs::path model_dir() {
    static const fs::path d = [] {
        fs::path dir = fs::temp_directory_path() / ("prism_deque_model_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        fs::copy_file(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "deque", dir / "deque",
                      fs::copy_options::overwrite_existing);
        return dir;
    }();
    return d;
}

struct DequeCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

DequeCtx& ctx() {
    static DequeCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <deque>\n"
                                  "int main() { std::deque<int> d{1}; return d[0] - 1; }\n",
                                  cxxh::asan_flags());
}

const std::vector<std::string>& flags() {
    static const std::vector<std::string> f{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                             "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    return f;
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("deque_trace.cpp"), "deque_trace", flags(), with_models);
}

}  // namespace

TEST_CASE("cxx deque: setup clang++ with ASan/UBSan and libstdc++") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxdeque-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with libstdc++ and ASan/UBSan not usable here");
}

TEST_CASE("cxx deque: trace matches libstdc++") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const std::string deps_anchor = fwd((model_dir() / "deque").string());
    std::vector<std::string> argv{c.cxx};
    argv.insert(argv.end(), flags().begin(), flags().end());
    argv.push_back("-isystem");
    argv.push_back(model_dir().string());
    argv.push_back("-M");
    argv.push_back((repo() / "tests" / "cxx_models" / "deque_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(argv, &deps));
    CHECK(fwd(deps).find(deps_anchor) != std::string::npos);
    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 20);
    CHECK(a == b);
}

TEST_CASE("cxx deque: push_back invalidates iterators as a heap use-after-free") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    // libstdc++ does not free a block on this push, so ASan stays quiet there.
    // The model deletes the iterator anchor; the load in operator* is the UAF.
    auto mod = c.tmp / "deque_trace.model";
    if (!fs::exists(mod)) mod = build_prog(c.cxx, c.tmp, true);
    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "uaf"}, &out, &err));
    CHECK((out + err).find("heap-use-after-free") != std::string::npos);
}
