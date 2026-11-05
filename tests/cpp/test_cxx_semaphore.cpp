// std::counting_semaphore model (src/prism/pir/models/cxx/semaphore).
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/semaphore.md has the line that links this file.
//
// release past the max template argument is a __prism_check and does not
// wrap the counter. try_acquire on zero is false and does not decrement, so
// a later release still adds a token. The header does not define a blocking
// acquire that returns success.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cctype>
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "semaphore"; }

struct SemCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

SemCtx& ctx() {
    static SemCtx c;
    return c;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const std::vector<std::string> kFlags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                       "-fno-sanitize-recover=all", "-g", "-O1", "-w"};

// acquire as its own name, not the suffix of try_acquire. A following '(' is
// a declaration. A blocking wait that returned success is not in this header.
bool declares_acquire(const std::string& text) {
    for (std::size_t i = 0; i + 7 <= text.size(); ++i) {
        if (text.compare(i, 7, "acquire") != 0) continue;
        if (i > 0) {
            const unsigned char prev = static_cast<unsigned char>(text[i - 1]);
            if (std::isalnum(prev) || prev == '_') continue;
        }
        std::size_t j = i + 7;
        if (j < text.size()) {
            const unsigned char next = static_cast<unsigned char>(text[j]);
            if (std::isalnum(next) || next == '_') continue;
        }
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        if (j < text.size() && text[j] == '(') return true;
    }
    return false;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    std::vector<std::string> with_models(kFlags);
    with_models.insert(with_models.end(), {"-isystem", cxxh::model_dir().string()});
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <semaphore>\n"
                                  "extern \"C\" void __prism_check(int, const char*, const char*) {}\n"
                                  "int main() {\n"
                                  "  std::counting_semaphore<1> s(0);\n"
                                  "  return s.max() == 1 ? 0 : 1;\n"
                                  "}\n",
                                  with_models);
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("semaphore_trace.cpp"), "semaphore_trace", kFlags, with_models);
}

}  // namespace

TEST_CASE("cxx semaphore model: release past max is a check and acquire is omitted") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_SEMAPHORE") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("class counting_semaphore") != std::string::npos);
    CHECK(text.find("using binary_semaphore = counting_semaphore<1>;") != std::string::npos);
    CHECK(text.find("#include <cstddef>") != std::string::npos);
    CHECK(text.find("#include <atomic>") == std::string::npos);
    CHECK(text.find("#include <latch>") == std::string::npos);
    CHECK(text.find("#include <thread>") == std::string::npos);
    CHECK(text.find("__prism_check") != std::string::npos);
    CHECK(text.find("counting_semaphore::release exceeds max") != std::string::npos);
    CHECK(text.find("if (__ok) _M_counter += __update;") != std::string::npos);
    CHECK_FALSE(declares_acquire(text));

    const auto fn = text.find("bool try_acquire()");
    REQUIRE(fn != std::string::npos);
    const auto end = text.find("\n};", fn);
    REQUIRE(end != std::string::npos);
    const std::string body = text.substr(fn, end - fn);
    CHECK(body.find("__prism_check") == std::string::npos);
    const auto ret_false = body.find("if (_M_counter <= 0) return false;");
    const auto dec = body.find("--_M_counter;");
    const auto ret_true = body.find("return true;");
    REQUIRE(ret_false != std::string::npos);
    REQUIRE(dec != std::string::npos);
    REQUIRE(ret_true != std::string::npos);
    CHECK(ret_false < dec);
    CHECK(dec < ret_true);
}

TEST_CASE("cxx semaphore model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxsemaphore-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and the semaphore model not usable here");
}

TEST_CASE("cxx semaphore model: zero try_acquire is false and release past max is a check") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), kFlags.begin(), kFlags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "semaphore_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto mod = build_prog(c.cxx, c.tmp, true);
    std::string out, err;
    REQUIRE(run_cmd({mod.string()}, &out, &err));
    CHECK(err.find("CHECK ") == std::string::npos);
    CHECK(out.find("zero=0\n") != std::string::npos);
    CHECK(out.find("took=1\n") != std::string::npos);
    CHECK(out.find("empty=0\n") != std::string::npos);

    std::string over_out, over_err;
    REQUIRE(run_cmd({mod.string(), "over"}, &over_out, &over_err));
    CHECK(over_err.find("CHECK CXX-SEMAPHORE-MAX:") != std::string::npos);
    CHECK(over_err.find("counting_semaphore::release exceeds max") != std::string::npos);
    CHECK(over_err.find("constructed outside") == std::string::npos);
    CHECK(over_out.find("over a=1 b=0\n") != std::string::npos);
    CHECK(over_out.find("over a=0") == std::string::npos);
    CHECK(over_out.find("over a=1 b=1") == std::string::npos);

    auto missing = c.tmp / "acquire.cpp";
    std::ofstream(missing) << "#include <semaphore>\n"
                                  "extern \"C\" void __prism_check(int, const char*, const char*) {}\n"
                                  "int main() {\n"
                                  "  std::counting_semaphore<1> s(0);\n"
                                  "  s.acquire();\n"
                                  "  return 0;\n"
                                  "}\n";
    std::vector<std::string> bad{c.cxx};
    bad.insert(bad.end(), kFlags.begin(), kFlags.end());
    bad.push_back("-isystem");
    bad.push_back(models.string());
    bad.push_back(missing.string());
    bad.push_back("-o");
    bad.push_back((c.tmp / "acquire_bad").string());
    std::string bad_err;
    CHECK_FALSE(run_cmd(bad, nullptr, &bad_err));
    CHECK(bad_err.find("acquire") != std::string::npos);

    fs::path lib;
    {
        fs::path out = c.tmp / "semaphore_trace.lib";
        std::vector<std::string> argv{c.cxx};
        argv.insert(argv.end(), kFlags.begin(), kFlags.end());
        argv.push_back((repo() / "tests" / "cxx_models" / "semaphore_trace.cpp").string());
        argv.push_back("-o");
        argv.push_back(out.string());
        if (run_cmd(argv)) lib = out;
    }
    if (!lib.empty()) {
        auto a = run_lines(lib);
        auto b = run_lines(mod);
        CHECK(a == b);
        CHECK(a.size() == 3);
    } else {
        prism_test::notrun("libstdc++ semaphore trace did not link: the libstdc++ comparison did not run; model trace still checked");
    }
}
