// from_chars_result and to_chars_result operator!= on the <charconv> model
// (src/prism/pir/models/cxx/charconv). Each body is the negation of the
// existing operator==. CMake's prism_tests source list is explicit (not a
// glob): this file is one line in _prism_test_sources, next to
// tests/cpp/test_cxx_charconv.cpp. The trace is
// tests/cxx_models/charconv_ne_trace.cpp.
//
// from_chars and to_chars for bool stay deleted. The header has no PROVED,
// no CLEAN, and no __builtin_trap(), and the new operators add none. The
// honesty note is docs/finish-notes/charconv-not-equal.md.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <algorithm>
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

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "charconv"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::size_t count_sub(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

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

std::string between_ops(const std::string& src, const std::string& eq_sig, const std::string& ne_sig) {
    const auto eq_at = src.find(eq_sig);
    const auto ne_at = src.find(ne_sig);
    REQUIRE(eq_at != std::string::npos);
    REQUIRE(ne_at != std::string::npos);
    REQUIRE(eq_at < ne_at);
    const std::string eq_body = function_body(src, eq_sig);
    const auto body_at = src.find(eq_body, eq_at);
    REQUIRE(body_at != std::string::npos);
    const auto after = body_at + eq_body.size();
    REQUIRE(after <= ne_at);
    return src.substr(after, ne_at - after);
}

void check_ne_body(const std::string& body) {
    CHECK(body.find("return !(__a == __b);") != std::string::npos);
    CHECK(count_sub(body, "==") == 1);
    CHECK(body.find("!=") == std::string::npos);
    CHECK(body.find("ptr") == std::string::npos);
    CHECK(body.find("ec") == std::string::npos);
    CHECK(body.find("*") == std::string::npos);
    CHECK(body.find("from_chars_result") == std::string::npos);
    CHECK(body.find("to_chars_result") == std::string::npos);
    CHECK(body.find("return false") == std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("float") == std::string::npos);
    CHECK(body.find("double") == std::string::npos);
}

void check_ne_comment(const std::string& note, const char* result_type) {
    CHECK(note.find("does not compare ptr or ec again") != std::string::npos);
    CHECK(note.find("does not dereference ptr") != std::string::npos);
    CHECK(note.find("A null ptr compares by address only") != std::string::npos);
    CHECK(note.find(std::string("Returns bool, not ") + result_type) != std::string::npos);
    CHECK(note.find("PROVED") == std::string::npos);
    CHECK(note.find("CLEAN") == std::string::npos);
    CHECK(note.find("__builtin_trap") == std::string::npos);
    CHECK(note.find("return __a.ptr") == std::string::npos);
    CHECK(note.find("*") == std::string::npos);
}

#ifdef _WIN32
constexpr const char* kModelExe = "charconv_ne.model.exe";
constexpr const char* kLibExe = "charconv_ne.libstd.exe";
#else
constexpr const char* kModelExe = "charconv_ne.model";
constexpr const char* kLibExe = "charconv_ne.libstd";
#endif

const std::vector<std::string> kFlags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                      "-fno-sanitize-recover=all", "-g", "-O1", "-w"};

fs::path trace_path() { return repo() / "tests" / "cxx_models" / "charconv_ne_trace.cpp"; }

struct NeCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
    bool built = false;
    std::vector<std::string> lib_lines;
    std::vector<std::string> model_lines;
};

NeCtx& ctx() {
    static NeCtx c;
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
                                  kFlags);
}

std::vector<std::string> compile_argv(const std::string& cxx, const fs::path& out, bool with_models) {
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), kFlags.begin(), kFlags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back((repo() / "src" / "prism" / "pir" / "models" / "cxx").string());
    }
    argv.push_back(trace_path().string());
    argv.push_back("-o");
    argv.push_back(out.string());
    return argv;
}

const std::vector<std::string> kExpect{
    "from same ne=0 eq=1 neg=1", "from self ne=0 eq=1 neg=1", "from ptr ne=1 eq=0 neg=1",
    "from ec ne=1 eq=0 neg=1",   "from both ne=1 eq=0 neg=1", "from null ne=0 eq=1 neg=1",
    "from nullec ne=1 eq=0 neg=1", "from nullp ne=1 eq=0 neg=1", "to same ne=0 eq=1 neg=1",
    "to self ne=0 eq=1 neg=1",   "to ptr ne=1 eq=0 neg=1",    "to ec ne=1 eq=0 neg=1",
    "to both ne=1 eq=0 neg=1",   "to null ne=0 eq=1 neg=1",   "to nullec ne=1 eq=0 neg=1",
    "to nullp ne=1 eq=0 neg=1",  "to cell=Q",
};

}  // namespace

TEST_CASE("cxx charconv model: operator!= negates operator== and does not repeat the header sentence") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#ifndef _PRISM_MODEL_CHARCONV") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_CHARCONV 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);

    // from_chars and to_chars for bool stay deleted: a call is ill-formed
    // and the unit falls back. The new operators add no PROVED, CLEAN, or
    // trap text.
    const std::string bool_from_del =
        "from_chars_result from_chars(const char*, const char*, bool&, int = 10) = delete;";
    const auto deleted = src.find(bool_from_del);
    REQUIRE(deleted != std::string::npos);
    CHECK(count_sub(src, "PROVED") == 0);
    CHECK(count_sub(src, "CLEAN") == 0);
    CHECK(count_sub(src, "__builtin_trap()") == 0);

    const std::string from_eq =
        "friend constexpr bool operator==(const from_chars_result& __a, const from_chars_result& __b) noexcept";
    const std::string from_ne =
        "friend constexpr bool operator!=(const from_chars_result& __a, const from_chars_result& __b) noexcept";
    const std::string to_eq =
        "friend constexpr bool operator==(const to_chars_result& __a, const to_chars_result& __b) noexcept";
    const std::string to_ne =
        "friend constexpr bool operator!=(const to_chars_result& __a, const to_chars_result& __b) noexcept";
    const auto from_eq_at = src.find(from_eq);
    const auto from_ne_at = src.find(from_ne);
    const auto to_struct = src.find("struct to_chars_result");
    const auto to_eq_at = src.find(to_eq);
    const auto to_ne_at = src.find(to_ne);
    REQUIRE(from_eq_at != std::string::npos);
    REQUIRE(from_ne_at != std::string::npos);
    REQUIRE(to_struct != std::string::npos);
    REQUIRE(to_eq_at != std::string::npos);
    REQUIRE(to_ne_at != std::string::npos);
    CHECK(from_eq_at < from_ne_at);
    CHECK(from_ne_at < to_struct);
    CHECK(to_struct < to_eq_at);
    CHECK(to_eq_at < to_ne_at);
    CHECK(to_ne_at < deleted);
    CHECK(count_sub(src, from_eq) == 1);
    CHECK(count_sub(src, from_ne) == 1);
    CHECK(count_sub(src, to_eq) == 1);
    CHECK(count_sub(src, to_ne) == 1);
    CHECK(src.find("from_chars_result operator!=") == std::string::npos);
    CHECK(src.find("to_chars_result operator!=") == std::string::npos);

    const std::string from_eq_body = function_body(src, from_eq);
    const std::string to_eq_body = function_body(src, to_eq);
    CHECK(from_eq_body.find("return __a.ptr == __b.ptr && __a.ec == __b.ec;") != std::string::npos);
    CHECK(to_eq_body.find("return __a.ptr == __b.ptr && __a.ec == __b.ec;") != std::string::npos);
    CHECK(count_sub(src, "return __a.ptr == __b.ptr && __a.ec == __b.ec;") == 2);
    CHECK(count_sub(src, "return !(__a == __b);") == 2);

    check_ne_comment(between_ops(src, from_eq, from_ne), "from_chars_result");
    check_ne_comment(between_ops(src, to_eq, to_ne), "to_chars_result");
    check_ne_body(function_body(src, from_ne));
    check_ne_body(function_body(src, to_ne));

    const std::string code = strip_comments(src);
    CHECK(count_sub(code, "operator!=") == 2);
    CHECK(code.find("chars_format") == std::string::npos);
    CHECK(code.find("float") == std::string::npos);
    CHECK(code.find("double") == std::string::npos);
    CHECK(code.find("long double") == std::string::npos);

    CHECK(src.find("to_chars(char*, char*, bool, int = 10) = delete;") != std::string::npos);
    CHECK(count_sub(src, bool_from_del) == 1);
    CHECK(count_sub(src, "= delete;") == 2);
    CHECK(src.find("bool& __value") == std::string::npos);
    CHECK(src.find("from_chars(const char* __first, const char* __last, _Tp& __value, int __base = 10)") !=
          std::string::npos);
    CHECK(src.find("to_chars(char* __first, char* __last, _Tp __value, int __base = 10)") != std::string::npos);

    const std::string trace = slurp(trace_path());
    CHECK(trace.find("#include <charconv>") != std::string::npos);
    CHECK(trace.find("from_chars(") == std::string::npos);
    CHECK(trace.find("to_chars(") == std::string::npos);
    CHECK(trace.find("nullptr") != std::string::npos);

    std::string cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_charconv.cpp\n") == 1);
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_charconv_ne.cpp\n") == 1);

    const std::string note = slurp(repo() / "docs" / "finish-notes" / "charconv-not-equal.md");
    CHECK(note.find("finish/cxx-charconv-ne") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(note.find("return !(__a == __b)") != std::string::npos);
    CHECK(note.find("A null ptr compares by address only") != std::string::npos);
    CHECK(note.find("tests/cpp/test_cxx_charconv_ne.cpp") != std::string::npos);
    CHECK(note.find("tests/cpp/test_cxx_charconv.cpp") != std::string::npos);
}

TEST_CASE("cxx charconv ne: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxcharconv-ne-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx charconv ne: inequality matches libstdc++ and does not load a pointer") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    if (!c.built) {
        const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
        std::vector<std::string> deps{c.cxx};
        deps.insert(deps.end(), kFlags.begin(), kFlags.end());
        deps.push_back("-isystem");
        deps.push_back(models.string());
        deps.push_back("-M");
        deps.push_back(trace_path().string());
        std::string deps_out;
        REQUIRE(run_cmd(deps, &deps_out));
        REQUIRE(fwd(deps_out).find(fwd((models / "charconv").string())) != std::string::npos);

        REQUIRE(run_cmd(compile_argv(c.cxx, c.tmp / kLibExe, false)));
        REQUIRE(run_cmd(compile_argv(c.cxx, c.tmp / kModelExe, true)));
        c.lib_lines = run_lines(c.tmp / kLibExe);
        c.model_lines = run_lines(c.tmp / kModelExe);
        c.built = true;
    }
    CHECK(c.lib_lines == c.model_lines);
    CHECK(c.model_lines == kExpect);
}
