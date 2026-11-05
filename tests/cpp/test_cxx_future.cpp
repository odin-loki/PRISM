// The std::future model header (src/prism/pir/models/cxx/future) is a file on
// the include path, same shape as tests/cpp/test_cxx_optional.cpp. CMake's
// prism_tests source list is explicit (not a glob): this file is one line in
// _prism_test_sources. docs/finish-notes/future.md has the line that links it.
// The trace is tests/cxx_models/future_trace.cpp.
//
// get() with no stored value is a __prism_check and does not return a
// value-initialised T. async and wait( are not defined.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include "prose_text.hpp"

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

fs::path future_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "future"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Comments and string literals are not definitions.
std::string strip_comments(const std::string& s) {
    std::string o;
    o.reserve(s.size());
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
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else ++i;
            }
            if (i < s.size()) ++i;
            continue;
        }
        o.push_back(s[i]);
        ++i;
    }
    return o;
}

std::string function_body(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    REQUIRE(at != std::string::npos);
    const auto brace = src.find('{', at);
    REQUIRE(brace != std::string::npos);
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    FAIL("unterminated function body");
    return {};
}

struct FutCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

FutCtx& ctx() {
    static FutCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <future>\n"
                                  "int main() {\n"
                                  "  std::promise<int> p;\n"
                                  "  std::future<int> f = p.get_future();\n"
                                  "  p.set_value(1);\n"
                                  "  return f.get() - 1;\n"
                                  "}\n",
                                  flags);
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("future_trace.cpp"), "future_trace", flags, with_models);
}

bool check_aborts(const fs::path& exe, const char* mode, const char* cls) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx future model: empty get() is a check and the header does not define async or wait(") {
    const fs::path header = future_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_FUTURE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_FUTURE") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("pir falls back") != std::string::npos);
    CHECK(src.find("fake synchronization") != std::string::npos);
    for (const char* inc : {"#include <memory>", "#include <thread>", "#include <functional>", "#include <mutex>"})
        CHECK(src.find(inc) == std::string::npos);

    const std::string get = function_body(src, "_Tp get()");
    const auto check = get.find("__prism_check(");
    const auto ret = get.find("return __outv");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(get.find("CXX-FUTURE-GET") != std::string::npos);
    CHECK(get.find("_M_state") != std::string::npos);
    CHECK(get.find("_M_retrieved") != std::string::npos);
    CHECK(get.find("return {}") == std::string::npos);
    CHECK(get.find("return _Tp") == std::string::npos);
    CHECK(get.find("_Tp{}") == std::string::npos);
    CHECK(get.find("_Tp()") == std::string::npos);
    CHECK(get.find("value_type{}") == std::string::npos);
    CHECK(get.find("value_type()") == std::string::npos);

    for (const char* sig : {"void set_value(const _Tp&", "void set_value(_Tp&&"}) {
        const std::string set = function_body(src, sig);
        const auto set_check = set.find("__prism_check(");
        const auto set_new = set.find("new _Tp");
        REQUIRE(set_check != std::string::npos);
        REQUIRE(set_new != std::string::npos);
        CHECK(set_check < set_new);
        CHECK(set.find("if (!__ok) return;") != std::string::npos);
        CHECK(set.find("FUNC-CONTRACT") != std::string::npos);
    }

    const std::string valid = function_body(src, "bool valid()");
    CHECK(valid.find("_M_state != nullptr") != std::string::npos);

    const std::string code = strip_comments(src);
    CHECK(code.find("async") == std::string::npos);
    CHECK(code.find("wait(") == std::string::npos);
    CHECK(code.find("wait_for") == std::string::npos);
    CHECK(code.find("wait_until") == std::string::npos);
    CHECK(code.find("packaged_task") == std::string::npos);
}

TEST_CASE("cxx future model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxfuture-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx future model: one stored value matches libstdc++ and empty get() is a check") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "future_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(future_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(b == std::vector<std::string>{"empty 0", "live 1", "got 42 0", "held 5"});

    CHECK(check_aborts(mod, "empty", "CXX-FUTURE-GET"));
    CHECK(check_aborts(mod, "twice", "FUNC-CONTRACT"));
    CHECK(check_aborts(mod, "get2", "CXX-FUTURE-GET"));
}
