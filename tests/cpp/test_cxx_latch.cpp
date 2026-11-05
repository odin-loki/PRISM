// The std::latch model header (src/prism/pir/models/cxx/latch) is a file on
// the include path. CMake's prism_tests source list is explicit (not a
// glob): this file is one line in _prism_test_sources. The trace is
// tests/cxx_models/latch_trace.cpp.
//
// count_down past zero is a __prism_check. try_wait is false while the
// count is positive. max() returns PTRDIFF_MAX and does not change the
// count. The header does not define wait(.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include "prose_text.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

fs::path latch_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "latch"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// `name` as its own identifier, then '('. try_wait does not define wait.
bool defines_call(std::string_view src, std::string_view name) {
    for (std::size_t i = 0; i + name.size() <= src.size(); ++i) {
        if (src.compare(i, name.size(), name) != 0) continue;
        if (i > 0 && is_ident(src[i - 1])) continue;
        std::size_t j = i + name.size();
        if (j < src.size() && is_ident(src[j])) continue;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) ++j;
        if (j < src.size() && src[j] == '(') return true;
    }
    return false;
}

struct LatchCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

LatchCtx& ctx() {
    static LatchCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <latch>\n"
                                  "int main() {\n"
                                  "  std::latch g(1);\n"
                                  "  g.count_down();\n"
                                  "  return g.try_wait() ? 0 : 1;\n"
                                  "}\n",
                                  flags);
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("latch_trace.cpp"), "latch_trace", flags, with_models);
}

bool over_is_check(const fs::path& exe) {
    std::string out, err;
    if (run_cmd({exe.string(), "over"}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK FUNC-CONTRACT:") == std::string::npos) return false;
    if (both.find("latch::count_down would pass below zero") == std::string::npos) return false;
    if (out.find("wrapped") != std::string::npos) return false;
    return true;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

}  // namespace

TEST_CASE("cxx latch model: count_down past zero is a check and wait is not defined") {
    const fs::path header = latch_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("_GLIBCXX_LATCH") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("ptrdiff_t _M_count;") != std::string::npos);

    const auto cd = src.find("void count_down(");
    REQUIRE(cd != std::string::npos);
    const auto tw = src.find("bool try_wait(", cd);
    REQUIRE(tw != std::string::npos);
    const std::string count_down = src.substr(cd, tw - cd);
    CHECK(count_down.find("__prism_check(__ok, \"FUNC-CONTRACT\", \"latch::count_down would pass below zero\")") !=
          std::string::npos);
    const auto check_at = count_down.find("__prism_check");
    const auto sub_at = count_down.find("_M_count -=");
    REQUIRE(check_at != std::string::npos);
    REQUIRE(sub_at != std::string::npos);
    CHECK(check_at < sub_at);
    CHECK(count_down.find("if (__ok) _M_count -= __n;") != std::string::npos);
    CHECK(count_down.find("_M_count -=", sub_at + 1) == std::string::npos);

    const auto tw_end = src.find('}', tw);
    REQUIRE(tw_end != std::string::npos);
    const std::string try_wait = src.substr(tw, tw_end - tw);
    CHECK(try_wait.find("return _M_count == 0;") != std::string::npos);
    CHECK(try_wait.find("-=") == std::string::npos);
    CHECK(try_wait.find("count_down") == std::string::npos);
    CHECK(try_wait.find("return true") == std::string::npos);

    CHECK(defines_call(src, "try_wait"));
    CHECK(defines_call(src, "count_down"));
    CHECK(defines_call(src, "max"));
    CHECK_FALSE(defines_call(src, "wait"));
    CHECK_FALSE(defines_call(src, "arrive_and_wait"));

    const auto mx = src.find("static constexpr ptrdiff_t max() noexcept");
    REQUIRE(mx != std::string::npos);
    CHECK(mx < cd);
    const auto mx_end = src.find('}', mx);
    REQUIRE(mx_end != std::string::npos);
    const std::string max_fn = src.substr(mx, mx_end - mx);
    CHECK(max_fn.find("return PTRDIFF_MAX;") != std::string::npos);
    CHECK(max_fn.find("_M_count") == std::string::npos);
    CHECK(max_fn.find("-=") == std::string::npos);
    CHECK(max_fn.find("count_down") == std::string::npos);
    CHECK(max_fn.find("wait") == std::string::npos);
    CHECK(src.find("max() is modelled") != std::string::npos);
    CHECK(src.find("wait and arrive_and_wait stay omitted") != std::string::npos);
    CHECK(src.find("max, wait, and arrive_and_wait are omitted") == std::string::npos);
    CHECK(src.find("#include <cstdint>") != std::string::npos);
}

TEST_CASE("cxx latch model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxlatch-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx latch model: try_wait is false while the count is positive") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "latch_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(latch_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "start 0"));
    CHECK(has_line(b, "mid 0"));
    CHECK(has_line(b, "zero 1"));
    CHECK(has_line(b, "empty 1"));
    CHECK(over_is_check(mod));
}
