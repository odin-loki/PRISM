// Differential test of the partial <algorithm> model
// (src/prism/pir/models/cxx/algorithm). The header is a file, so
// `#include <algorithm>` is not EISDIR. CMake's prism_tests source list is
// explicit (not a glob).
//
// min and max return one of the two arguments. Equal arguments return the
// first. An inverted clamp is __prism_check then __builtin_trap, and that
// path does not return v. min of the extreme ints is not 0.
// min_element and max_element take two pointers. An inverted range is
// __prism_check then __builtin_trap and does not return a pointer. An empty
// range returns last and does not read an element. Equal elements keep the
// first. find takes two pointers and a const T&. An inverted range is
// __prism_check then __builtin_trap and does not read or invent a T. An
// empty range returns last and does not read. The result is the first
// operator== match, or last. find does not allocate. find_if takes two
// pointers and bool (*)(const T&). A null predicate is __prism_check then
// __builtin_trap and does not call. An inverted range is __prism_check
// then __builtin_trap and does not read or invent a T. An empty range
// returns last and does not read or call. count takes two pointers and a
// const T&. An inverted range is __prism_check then __builtin_trap and
// does not read. An empty range returns 0 and does not read. A tally that
// would not fit in ptrdiff_t is __prism_check then __builtin_trap and does
// not return a wrapped count. This is not PROVED and not CLEAN. sort,
// copy, fill, and for_each are not declared.

#include <doctest/doctest.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  include "support/proctree.hpp"
#endif

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "algorithm"; }

struct AlgCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

AlgCtx& ctx() {
    static AlgCtx c;
    return c;
}

bool run_cmd(const std::vector<std::string>& argv, std::string* out = nullptr, std::string* err = nullptr) {
#ifndef _WIN32
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int pipe_out[2]{-1, -1};
    int pipe_err[2]{-1, -1};
    if (out && pipe(pipe_out) != 0) return false;
    if (err && pipe(pipe_err) != 0) {
        if (out) {
            close(pipe_out[0]);
            close(pipe_out[1]);
        }
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
        posix_spawn_file_actions_addclose(&actions, pipe_out[0]);
        posix_spawn_file_actions_adddup2(&actions, pipe_out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_out[1]);
    }
    if (err) {
        posix_spawn_file_actions_addclose(&actions, pipe_err[0]);
        posix_spawn_file_actions_adddup2(&actions, pipe_err[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_err[1]);
    }
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (out) {
            close(pipe_out[0]);
            close(pipe_out[1]);
        }
        if (err) {
            close(pipe_err[0]);
            close(pipe_err[1]);
        }
        return false;
    }
    posix_spawn_file_actions_destroy(&actions);
    if (out) {
        close(pipe_out[1]);
        out->clear();
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(pipe_out[0], buf, sizeof buf)) > 0) out->append(buf, static_cast<size_t>(n));
        close(pipe_out[0]);
    }
    if (err) {
        close(pipe_err[1]);
        err->clear();
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(pipe_err[0], buf, sizeof buf)) > 0) err->append(buf, static_cast<size_t>(n));
        close(pipe_err[0]);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#else
    prism::qa::RunOpts o;
    o.timeout_s = 300;
    auto r = prism::qa::run_tree(argv, o);
    if (out) {
        *out = r.out;
        std::erase(*out, '\r');  // text-mode stdout on Windows is CRLF
    }
    if (err) *err = r.err;
    return !r.start_failed && !r.timed_out && r.rc == 0;
#endif
}

std::string fwd(std::string s) {
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}

#ifdef _WIN32
constexpr const char* kProbeExe = "probe.exe";
#else
constexpr const char* kProbeExe = "probe";
#endif

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <algorithm>\n#include <climits>\n"
                            "int main() {\n"
                            "  if (std::min(7, 3) != 3) return 1;\n"
                            "  if (std::max(3, 7) != 7) return 2;\n"
                            "  if (std::clamp(5, 3, 7) != 5) return 3;\n"
                            "  if (std::min(INT_MIN, INT_MAX) == 0) return 4;\n"
                            "  return 0;\n"
                            "}\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build{cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.push_back("-o");
    build.push_back((tmp / kProbeExe).string());
    if (!run_cmd(build)) return false;
    return run_cmd({(tmp / kProbeExe).string()});
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (with_models ? "algorithm_trace.model" : "algorithm_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "algorithm_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

std::vector<std::string> run_lines(const fs::path& exe) {
    std::string stdout_text;
    REQUIRE(run_cmd({exe.string()}, &stdout_text));
    std::vector<std::string> lines;
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

bool syntax_only(const std::string& cxx, const fs::path& src, bool with_models, std::string* err) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only", "-w"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv, nullptr, err);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("cxx algorithm model: header is a file and returns an argument") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_ALGORITHM") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("not a proof that a range is sorted") != std::string::npos);
    CHECK(text.find("fails to compile") != std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int ok, const char* cls, const char* msg);") != std::string::npos);
    CHECK(text.find("__prism_check(int ok, const char* cls, const char* msg) {") == std::string::npos);
    CHECK(text.find("not PROVED") != std::string::npos);
    CHECK(text.find("not CLEAN") != std::string::npos);
    for (const char* inc : {"<memory>", "<functional>", "<string>", "<iterator>", "<ranges>", "<vector>", "<compare>"})
        CHECK(text.find(inc) == std::string::npos);
    {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            CHECK(line.compare(i, 8, "#include") != 0);
        }
    }
    for (const char* name : {"sort(", "copy(", "fill(", "for_each(", "count_if", "find_if_not"})
        CHECK(text.find(name) == std::string::npos);

    const auto min_at = text.find("constexpr const _Tp& min(");
    const auto max_at = text.find("constexpr const _Tp& max(", min_at);
    const auto clamp_at = text.find("constexpr const _Tp& clamp(", max_at);
    REQUIRE(min_at != std::string::npos);
    REQUIRE(max_at != std::string::npos);
    REQUIRE(clamp_at != std::string::npos);
    const std::string min_body = text.substr(min_at, max_at - min_at);
    const std::string max_body = text.substr(max_at, clamp_at - max_at);
    const auto min_cond = min_body.find("!(__b < __a)");
    const auto min_a = min_body.find("return __a");
    const auto min_b = min_body.find("return __b");
    REQUIRE(min_cond != std::string::npos);
    REQUIRE(min_a != std::string::npos);
    REQUIRE(min_b != std::string::npos);
    CHECK(min_cond < min_a);
    CHECK(min_a < min_b);
    CHECK(min_body.find("+") == std::string::npos);
    const auto max_cond = max_body.find("!(__a < __b)");
    const auto max_a = max_body.find("return __a");
    const auto max_b = max_body.find("return __b");
    REQUIRE(max_cond != std::string::npos);
    REQUIRE(max_a != std::string::npos);
    REQUIRE(max_b != std::string::npos);
    CHECK(max_cond < max_a);
    CHECK(max_a < max_b);
    CHECK(max_body.find("+") == std::string::npos);

    const auto bad = text.find("if (__hi < __lo)", clamp_at);
    const auto lo_if = text.find("if (__v < __lo)", bad);
    REQUIRE(bad != std::string::npos);
    REQUIRE(lo_if != std::string::npos);
    const std::string bad_body = text.substr(bad, lo_if - bad);
    const auto check = bad_body.find("__prism_check");
    const auto trap = bad_body.find("__builtin_trap");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(check < trap);
    CHECK(bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(bad_body.find("clamp: hi < lo") != std::string::npos);
    CHECK(bad_body.find("return") == std::string::npos);
    const std::string rest = text.substr(lo_if);
    const auto ret_lo = rest.find("return __lo");
    const auto ret_hi = rest.find("return __hi");
    const auto ret_v = rest.find("return __v");
    REQUIRE(ret_lo != std::string::npos);
    REQUIRE(ret_hi != std::string::npos);
    REQUIRE(ret_v != std::string::npos);
    CHECK(ret_lo < ret_hi);
    CHECK(ret_hi < ret_v);

    const auto min_el_at = text.find("constexpr _Tp* min_element(_Tp* __first, _Tp* __last)");
    const auto max_el_at = text.find("constexpr _Tp* max_element(_Tp* __first, _Tp* __last)", min_el_at);
    const auto eq_at = text.find("concept _Prism_equality_comparable");
    const auto find_at = text.find("constexpr _Tp* find(_Tp* __first, _Tp* __last, const _Tp& __value)");
    REQUIRE(min_el_at != std::string::npos);
    REQUIRE(max_el_at != std::string::npos);
    REQUIRE(eq_at != std::string::npos);
    REQUIRE(find_at != std::string::npos);
    CHECK(min_el_at > clamp_at);
    CHECK(max_el_at < eq_at);
    CHECK(eq_at < find_at);
    CHECK(text.find("min_element(_Tp* __first, _Tp* __last,") == std::string::npos);
    CHECK(text.find("max_element(_Tp* __first, _Tp* __last,") == std::string::npos);
    CHECK(text.find("find(_Tp* __first, _Tp* __last, const _Tp& __value,") == std::string::npos);
    const std::string min_el_body = text.substr(min_el_at, max_el_at - min_el_at);
    const std::string max_el_body = text.substr(max_el_at, eq_at - max_el_at);
    const auto check_range = [](const std::string& body, const char* msg) {
        const auto bad = body.find("if (__last < __first)");
        const auto empty = body.find("if (__first == __last) return __last;");
        REQUIRE(bad != std::string::npos);
        REQUIRE(empty != std::string::npos);
        CHECK(bad < empty);
        const std::string bad_body = body.substr(bad, empty - bad);
        const auto check = bad_body.find("__prism_check");
        const auto trap = bad_body.find("__builtin_trap");
        REQUIRE(check != std::string::npos);
        REQUIRE(trap != std::string::npos);
        CHECK(check < trap);
        CHECK(bad_body.find("FUNC-CONTRACT") != std::string::npos);
        CHECK(bad_body.find(msg) != std::string::npos);
        CHECK(bad_body.find("return") == std::string::npos);
        CHECK(bad_body.find("*__") == std::string::npos);
        const std::string before_read = body.substr(0, empty);
        CHECK(before_read.find("*__") == std::string::npos);
        CHECK(body.find("_Tp{") == std::string::npos);
        CHECK(body.find("_Tp(") == std::string::npos);
        CHECK(body.find("_Tp __") == std::string::npos);
        CHECK(body.find("<=") == std::string::npos);
        const auto scan = body.find("while (__first != __last)", empty);
        REQUIRE(scan != std::string::npos);
        CHECK(body.find("*__", scan) != std::string::npos);
    };
    check_range(min_el_body, "min_element: last < first");
    check_range(max_el_body, "max_element: last < first");
    CHECK(min_el_body.find("if (*__first < *__result)") != std::string::npos);
    CHECK(max_el_body.find("if (*__result < *__first)") != std::string::npos);

    const auto find_if_at =
        text.find("constexpr _Tp* find_if(_Tp* __first, _Tp* __last, bool (*__pred)(const _Tp&))");
    const auto count_at =
        text.find("constexpr __PTRDIFF_TYPE__ count(_Tp* __first, _Tp* __last, const _Tp& __value)");
    REQUIRE(find_if_at != std::string::npos);
    REQUIRE(count_at != std::string::npos);
    CHECK(find_at < find_if_at);
    CHECK(find_if_at < count_at);
    CHECK(text.substr(0, count_at).find("return 0") == std::string::npos);
    const auto find_if_sig = text.find("find_if(", find_if_at);
    const auto count_sig = text.find("count(", count_at);
    REQUIRE(find_if_sig != std::string::npos);
    REQUIRE(count_sig != std::string::npos);
    CHECK(text.find("find_if(", find_if_sig + 8) == std::string::npos);
    CHECK(text.find("count(", count_sig + 6) == std::string::npos);

    const std::string find_body = text.substr(find_at, find_if_at - find_at);
    const auto find_sig = find_body.find("find(");
    REQUIRE(find_sig != std::string::npos);
    CHECK(find_body.find("find(", find_sig + 1) == std::string::npos);
    const auto find_bad = find_body.find("if (__last < __first)");
    const auto find_empty = find_body.find("if (__first == __last) return __last;");
    REQUIRE(find_bad != std::string::npos);
    REQUIRE(find_empty != std::string::npos);
    CHECK(find_bad < find_empty);
    const std::string find_bad_body = find_body.substr(find_bad, find_empty - find_bad);
    const auto find_check = find_bad_body.find("__prism_check");
    const auto find_trap = find_bad_body.find("__builtin_trap");
    REQUIRE(find_check != std::string::npos);
    REQUIRE(find_trap != std::string::npos);
    CHECK(find_check < find_trap);
    CHECK(find_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(find_bad_body.find("find: last < first") != std::string::npos);
    CHECK(find_bad_body.find("return") == std::string::npos);
    CHECK(find_bad_body.find("*__") == std::string::npos);
    CHECK(find_bad_body.find("__value") == std::string::npos);
    CHECK(find_bad_body.find("_Tp{") == std::string::npos);
    CHECK(find_bad_body.find("_Tp(") == std::string::npos);
    CHECK(find_bad_body.find("_Tp __") == std::string::npos);
    CHECK(find_body.substr(0, find_empty).find("*__") == std::string::npos);
    const auto find_scan = find_body.find("while (__first != __last)", find_empty);
    REQUIRE(find_scan != std::string::npos);
    const std::string find_between = find_body.substr(find_empty, find_scan - find_empty);
    CHECK(find_between.find("*__") == std::string::npos);
    CHECK(find_between.find("__value") == std::string::npos);
    CHECK(find_body.find("_Tp{") == std::string::npos);
    CHECK(find_body.find("_Tp(") == std::string::npos);
    CHECK(find_body.find("_Tp __") == std::string::npos);
    CHECK(find_body.find("new") == std::string::npos);
    CHECK(find_body.find("malloc") == std::string::npos);
    CHECK(find_body.find("calloc") == std::string::npos);
    CHECK(find_body.find("realloc") == std::string::npos);
    const auto find_eq = find_body.find("if (*__first == __value)", find_scan);
    REQUIRE(find_eq != std::string::npos);
    const auto find_hit = find_body.find("return __first", find_eq);
    REQUIRE(find_hit != std::string::npos);
    const auto find_bump = find_body.find("++__first", find_hit);
    REQUIRE(find_bump != std::string::npos);
    const auto find_end = find_body.find("return __last", find_bump);
    REQUIRE(find_end != std::string::npos);
    CHECK(find_scan < find_eq);
    CHECK(find_eq < find_hit);
    CHECK(find_hit < find_bump);
    CHECK(find_bump < find_end);

    const std::string find_if_body = text.substr(find_if_at, count_at - find_if_at);
    const auto fi_brace = find_if_body.find('{');
    REQUIRE(fi_brace != std::string::npos);
    const std::string find_if_fn = find_if_body.substr(fi_brace);
    const auto fi_null = find_if_fn.find("if (__pred == nullptr)");
    const auto fi_bad = find_if_fn.find("if (__last < __first)");
    const auto fi_empty = find_if_fn.find("if (__first == __last) return __last;");
    REQUIRE(fi_null != std::string::npos);
    REQUIRE(fi_bad != std::string::npos);
    REQUIRE(fi_empty != std::string::npos);
    CHECK(fi_null < fi_bad);
    CHECK(fi_bad < fi_empty);
    const std::string fi_null_body = find_if_fn.substr(fi_null, fi_bad - fi_null);
    const auto fi_null_check = fi_null_body.find("__prism_check");
    const auto fi_null_trap = fi_null_body.find("__builtin_trap");
    REQUIRE(fi_null_check != std::string::npos);
    REQUIRE(fi_null_trap != std::string::npos);
    CHECK(fi_null_check < fi_null_trap);
    CHECK(fi_null_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(fi_null_body.find("find_if: null predicate") != std::string::npos);
    CHECK(fi_null_body.find("return") == std::string::npos);
    CHECK(fi_null_body.find("*__") == std::string::npos);
    CHECK(fi_null_body.find("__pred(") == std::string::npos);
    const std::string fi_bad_body = find_if_fn.substr(fi_bad, fi_empty - fi_bad);
    const auto fi_bad_check = fi_bad_body.find("__prism_check");
    const auto fi_bad_trap = fi_bad_body.find("__builtin_trap");
    REQUIRE(fi_bad_check != std::string::npos);
    REQUIRE(fi_bad_trap != std::string::npos);
    CHECK(fi_bad_check < fi_bad_trap);
    CHECK(fi_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(fi_bad_body.find("find_if: last < first") != std::string::npos);
    CHECK(fi_bad_body.find("return") == std::string::npos);
    CHECK(fi_bad_body.find("*__") == std::string::npos);
    CHECK(fi_bad_body.find("__pred(") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp{") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp(") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp __") == std::string::npos);
    CHECK(find_if_fn.substr(0, fi_empty).find("*__") == std::string::npos);
    CHECK(find_if_fn.substr(0, fi_empty).find("__pred(") == std::string::npos);
    const auto fi_scan = find_if_fn.find("while (__first != __last)", fi_empty);
    REQUIRE(fi_scan != std::string::npos);
    const std::string fi_between = find_if_fn.substr(fi_empty, fi_scan - fi_empty);
    CHECK(fi_between.find("*__") == std::string::npos);
    CHECK(fi_between.find("__pred(") == std::string::npos);
    const auto fi_call = find_if_fn.find("__pred(*__first)", fi_scan);
    REQUIRE(fi_call != std::string::npos);
    const auto fi_hit = find_if_fn.find("return __first", fi_call);
    REQUIRE(fi_hit != std::string::npos);
    const auto fi_bump = find_if_fn.find("++__first", fi_hit);
    REQUIRE(fi_bump != std::string::npos);
    const auto fi_end = find_if_fn.find("return __last", fi_bump);
    REQUIRE(fi_end != std::string::npos);
    CHECK(fi_scan < fi_call);
    CHECK(fi_call < fi_hit);
    CHECK(fi_hit < fi_bump);
    CHECK(fi_bump < fi_end);
    CHECK(find_if_fn.find("find_if(", fi_call) == std::string::npos);
    CHECK(find_if_body.find("_Tp{") == std::string::npos);
    CHECK(find_if_body.find("_Tp(") == std::string::npos);
    CHECK(find_if_body.find("_Tp __") == std::string::npos);
    CHECK(find_if_body.find("new") == std::string::npos);
    CHECK(find_if_body.find("malloc") == std::string::npos);
    CHECK(find_if_body.find("return 0") == std::string::npos);

    const std::string count_body = text.substr(count_at);
    const auto count_brace = count_body.find('{');
    REQUIRE(count_brace != std::string::npos);
    const std::string count_fn = count_body.substr(count_brace);
    const auto count_bad = count_fn.find("if (__last < __first)");
    const auto count_empty = count_fn.find("if (__first == __last) return 0;");
    REQUIRE(count_bad != std::string::npos);
    REQUIRE(count_empty != std::string::npos);
    CHECK(count_bad < count_empty);
    const std::string count_bad_body = count_fn.substr(count_bad, count_empty - count_bad);
    const auto count_check = count_bad_body.find("__prism_check");
    const auto count_trap = count_bad_body.find("__builtin_trap");
    REQUIRE(count_check != std::string::npos);
    REQUIRE(count_trap != std::string::npos);
    CHECK(count_check < count_trap);
    CHECK(count_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(count_bad_body.find("count: last < first") != std::string::npos);
    CHECK(count_bad_body.find("return") == std::string::npos);
    CHECK(count_bad_body.find("*__") == std::string::npos);
    CHECK(count_bad_body.find("__value") == std::string::npos);
    CHECK(count_bad_body.find("_Tp{") == std::string::npos);
    CHECK(count_bad_body.find("_Tp(") == std::string::npos);
    CHECK(count_bad_body.find("_Tp __") == std::string::npos);
    CHECK(count_fn.substr(0, count_empty).find("*__") == std::string::npos);
    CHECK(count_fn.substr(0, count_empty).find("__value") == std::string::npos);
    const auto count_zero = count_fn.find("__n = 0", count_empty);
    const auto count_scan = count_fn.find("while (__first != __last)", count_empty);
    REQUIRE(count_zero != std::string::npos);
    REQUIRE(count_scan != std::string::npos);
    CHECK(count_empty < count_zero);
    CHECK(count_zero < count_scan);
    const std::string count_between = count_fn.substr(count_empty, count_scan - count_empty);
    CHECK(count_between.find("*__") == std::string::npos);
    CHECK(count_between.find("__value") == std::string::npos);
    const auto count_eq = count_fn.find("if (*__first == __value)", count_scan);
    REQUIRE(count_eq != std::string::npos);
    const auto count_fit =
        count_fn.find("if (__n == static_cast<__PTRDIFF_TYPE__>(__PTRDIFF_MAX__))", count_eq);
    REQUIRE(count_fit != std::string::npos);
    const auto count_inc = count_fn.find("++__n", count_fit);
    REQUIRE(count_inc != std::string::npos);
    const std::string count_fit_body = count_fn.substr(count_fit, count_inc - count_fit);
    const auto count_fit_check = count_fit_body.find("__prism_check");
    const auto count_fit_trap = count_fit_body.find("__builtin_trap");
    REQUIRE(count_fit_check != std::string::npos);
    REQUIRE(count_fit_trap != std::string::npos);
    CHECK(count_fit_check < count_fit_trap);
    CHECK(count_fit_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(count_fit_body.find("count: result does not fit in ptrdiff_t") != std::string::npos);
    CHECK(count_fit_body.find("return") == std::string::npos);
    CHECK(count_fit_body.find("*__") == std::string::npos);
    CHECK(count_fit_body.find("++__n") == std::string::npos);
    CHECK(count_fit_body.find("++__first") == std::string::npos);
    const auto count_bump = count_fn.find("++__first", count_inc);
    REQUIRE(count_bump != std::string::npos);
    const auto count_ret = count_fn.find("return __n", count_bump);
    REQUIRE(count_ret != std::string::npos);
    CHECK(count_scan < count_eq);
    CHECK(count_eq < count_fit);
    CHECK(count_fit < count_inc);
    CHECK(count_inc < count_bump);
    CHECK(count_bump < count_ret);
    CHECK(count_body.find("_Tp{") == std::string::npos);
    CHECK(count_body.find("_Tp(") == std::string::npos);
    CHECK(count_body.find("_Tp __") == std::string::npos);
    CHECK(count_body.find("new") == std::string::npos);
    CHECK(count_body.find("malloc") == std::string::npos);
    CHECK(count_body.find("calloc") == std::string::npos);
    CHECK(count_body.find("realloc") == std::string::npos);
}

TEST_CASE("cxx algorithm model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxalgorithm-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        for (const char* name : {"clang++-18", "clang++"}) {
            if (run_cmd({name, "--version"})) {
                c.cxx = name;
                break;
            }
        }
        if (!c.cxx.empty()) c.ok = probe_clang_asan(c.cxx, c.tmp);
    }
    if (!c.ok) MESSAGE("NOTRUN: clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx algorithm model: trace matches libstdc++ and an inverted clamp does not return v") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "algorithm_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "min_eq=1"));
    CHECK(has_line(b, "max_eq=1"));
    CHECK(has_line(b, "min_lo=2"));
    CHECK(has_line(b, "max_hi=9"));
    CHECK(has_line(b, "clamp_mid=5 id=1"));
    CHECK(has_line(b, "clamp_lo=2 id=1"));
    CHECK(has_line(b, "clamp_hi=9 id=1"));
    CHECK(has_line(b, "wide_min_is0=0 wide_min_is_min=1"));
    CHECK(has_line(b, "wide_max_is0=0 wide_max_is_max=1"));
    CHECK(has_line(b, "clamp_inside_zero=1"));
    CHECK(has_line(b, "ord_eq_first=1 ord_max=2"));
    CHECK(has_line(b, "ptr_eq_first=1"));
    CHECK(has_line(b, "el_min=1 el_min_i=2"));
    CHECK(has_line(b, "el_max=9 el_max_i=0"));
    CHECK(has_line(b, "el_empty=1"));
    CHECK(has_line(b, "el_tie=1"));
    CHECK(has_line(b, "el_one=1"));
    CHECK(has_line(b, "el_null=1"));
    CHECK(has_line(b, "el_ord_min=1 el_ord_max_i=0"));
    CHECK(has_line(b, "el_nodef_min=1 el_nodef_max_i=0 el_nodef_empty=1"));
    CHECK(has_line(b, "find_hit=1 find_hit_i=2"));
    CHECK(has_line(b, "find_first_i=1"));
    CHECK(has_line(b, "find_front_i=0"));
    CHECK(has_line(b, "find_miss=1"));
    CHECK(has_line(b, "find_empty=1"));
    CHECK(has_line(b, "find_one=1"));
    CHECK(has_line(b, "find_null=1"));
    CHECK(has_line(b, "find_keep=1"));
    CHECK(has_line(b, "find_eq_i=1 find_eq_id=1 find_eq_miss=1 find_eq_empty=1"));
    CHECK(has_line(b, "find_nodef=1 find_nodef_i=1 find_nodef_empty=1"));
    CHECK(has_line(b, "find_if_hit=1 find_if_hit_i=2 find_if_hit_calls=3"));
    CHECK(has_line(b, "find_if_first_i=1 find_if_first_calls=2"));
    CHECK(has_line(b, "find_if_front_i=0 find_if_front_calls=1"));
    CHECK(has_line(b, "find_if_miss=1 find_if_miss_calls=4"));
    CHECK(has_line(b, "find_if_empty=1 find_if_empty_calls=0"));
    CHECK(has_line(b, "find_if_tie=1 find_if_tie_calls=1"));
    CHECK(has_line(b, "find_if_one=1"));
    CHECK(has_line(b, "find_if_null=1"));
    CHECK(has_line(b, "find_if_keep=1"));
    CHECK(has_line(b, "find_if_eq_i=1 find_if_eq_empty=1"));
    CHECK(has_line(b, "find_if_nodef_i=1 find_if_nodef_empty=1"));
    CHECK(has_line(b, "find_if_pred_i=1 find_if_pred_empty=1"));
    CHECK(has_line(b, "count_four=2"));
    CHECK(has_line(b, "count_one=1"));
    CHECK(has_line(b, "count_nine=1"));
    CHECK(has_line(b, "count_miss=0"));
    CHECK(has_line(b, "count_empty=0"));
    CHECK(has_line(b, "count_tie=2"));
    CHECK(has_line(b, "count_one_hit=1"));
    CHECK(has_line(b, "count_one_miss=0"));
    CHECK(has_line(b, "count_null=0"));
    CHECK(has_line(b, "count_eq=2 count_eq_miss=0 count_eq_empty=0"));
    CHECK(has_line(b, "count_nodef=1 count_nodef_three=2 count_nodef_empty=0"));
    CHECK(has_line(b, "count_keep=1"));

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "invert"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("clamp: hi < lo") != std::string::npos);
    CHECK(out.find("value=") == std::string::npos);

    for (const char* arg : {"invert-min", "invert-max"}) {
        std::string el_out, el_err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &el_out, &el_err));
        const std::string el_both = el_out + el_err;
        const char* el_msg =
            std::string(arg) == "invert-min" ? "min_element: last < first" : "max_element: last < first";
        CHECK(el_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
        CHECK(el_both.find(el_msg) != std::string::npos);
        CHECK(el_out.find("value=") == std::string::npos);
    }

    std::string find_out, find_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-find"}, &find_out, &find_err));
    const std::string find_both = find_out + find_err;
    CHECK(find_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(find_both.find("find: last < first") != std::string::npos);
    CHECK(find_out.find("value=") == std::string::npos);

    std::string fi_out, fi_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-find-if"}, &fi_out, &fi_err));
    const std::string fi_both = fi_out + fi_err;
    CHECK(fi_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(fi_both.find("find_if: last < first") != std::string::npos);
    CHECK(fi_both.find("called") == std::string::npos);
    CHECK(fi_out.find("value=") == std::string::npos);

    std::string null_out, null_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-find-if"}, &null_out, &null_err));
    const std::string null_both = null_out + null_err;
    CHECK(null_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(null_both.find("find_if: null predicate") != std::string::npos);
    CHECK(null_out.find("value=") == std::string::npos);

    std::string null_empty_out, null_empty_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-find-if-empty"}, &null_empty_out, &null_empty_err));
    const std::string null_empty_both = null_empty_out + null_empty_err;
    CHECK(null_empty_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(null_empty_both.find("find_if: null predicate") != std::string::npos);
    CHECK(null_empty_out.find("value=") == std::string::npos);

    std::string count_out, count_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-count"}, &count_out, &count_err));
    const std::string count_both = count_out + count_err;
    CHECK(count_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(count_both.find("count: last < first") != std::string::npos);
    CHECK(count_out.find("value=") == std::string::npos);

    const fs::path omitted = c.tmp / "omitted.cpp";
    std::ofstream(omitted) << "#include <algorithm>\n"
                              "int use(int* a, int n) {\n"
                              "  std::sort(a, a + n);\n"
                              "  std::copy(a, a + n, a);\n"
                              "  std::fill(a, a + n, 0);\n"
                              "  std::for_each(a, a + n, [](int) {});\n"
                              "  return a[0];\n"
                              "}\n";
    std::string omitted_err;
    CHECK(syntax_only(c.cxx, omitted, false, &omitted_err));
    CHECK_FALSE(syntax_only(c.cxx, omitted, true, &omitted_err));

    const fs::path comp = c.tmp / "comp.cpp";
    std::ofstream(comp) << "#include <algorithm>\n"
                           "int use(int* a, int n) {\n"
                           "  auto less = [](int x, int y) { return x < y; };\n"
                           "  return *std::min_element(a, a + n, less) +\n"
                           "         *std::max_element(a, a + n, less);\n"
                           "}\n";
    std::string comp_err;
    CHECK(syntax_only(c.cxx, comp, false, &comp_err));
    CHECK_FALSE(syntax_only(c.cxx, comp, true, &comp_err));

    const fs::path inverted = c.tmp / "invert_const.cpp";
    std::ofstream(inverted) << "#include <algorithm>\n"
                               "static_assert(std::clamp(0, 2, 1) == 0);\n";
    std::string inverted_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted, true, &inverted_err));

    const fs::path inverted_el = c.tmp / "invert_el_const.cpp";
    std::ofstream(inverted_el) << "#include <algorithm>\n"
                                  "constexpr int a[] = {1, 2, 3};\n"
                                  "static_assert(std::min_element(a + 2, a) == a);\n"
                                  "static_assert(std::max_element(a + 2, a) == a);\n";
    std::string inverted_el_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_el, true, &inverted_el_err));

    const fs::path find_ok = c.tmp / "find_ok.cpp";
    std::ofstream(find_ok) << "#include <algorithm>\n"
                              "int use(int* a, int n, const int& v) {\n"
                              "  int* p = std::find(a, a + n, v);\n"
                              "  if (p == a + n) return 0;\n"
                              "  return *p;\n"
                              "}\n";
    std::string find_ok_err;
    CHECK(syntax_only(c.cxx, find_ok, false, &find_ok_err));
    CHECK(syntax_only(c.cxx, find_ok, true, &find_ok_err));

    const fs::path find_long = c.tmp / "find_long.cpp";
    std::ofstream(find_long) << "#include <algorithm>\n"
                                "long use(int* a, int n) {\n"
                                "  return *std::find(a, a + n, 0L);\n"
                                "}\n";
    std::string find_long_err;
    CHECK(syntax_only(c.cxx, find_long, false, &find_long_err));
    CHECK_FALSE(syntax_only(c.cxx, find_long, true, &find_long_err));

    const fs::path find_const = c.tmp / "find_const.cpp";
    std::ofstream(find_const) << "#include <algorithm>\n"
                                 "int use(const int* a, int n, int v) {\n"
                                 "  return *std::find(a, a + n, v);\n"
                                 "}\n";
    std::string find_const_err;
    CHECK(syntax_only(c.cxx, find_const, false, &find_const_err));
    CHECK_FALSE(syntax_only(c.cxx, find_const, true, &find_const_err));

    const fs::path find_if = c.tmp / "find_if.cpp";
    std::ofstream(find_if) << "#include <algorithm>\n"
                              "int use(int* a, int n) {\n"
                              "  return *std::find_if(a, a + n, [](int x) { return x == 0; });\n"
                              "}\n";
    std::string find_if_err;
    CHECK(syntax_only(c.cxx, find_if, false, &find_if_err));
    CHECK_FALSE(syntax_only(c.cxx, find_if, true, &find_if_err));

    const fs::path inverted_find = c.tmp / "invert_find_const.cpp";
    std::ofstream(inverted_find) << "#include <algorithm>\n"
                                    "constexpr int inverted() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  return *std::find(a + 2, a, 1);\n"
                                    "}\n"
                                    "static_assert(inverted() == 0);\n";
    std::string inverted_find_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_find, true, &inverted_find_err));

    const fs::path find_if_fn = c.tmp / "find_if_fn.cpp";
    std::ofstream(find_if_fn) << "#include <algorithm>\n"
                                 "bool is_zero(const int& x) { return x == 0; }\n"
                                 "int use(int* a, int n) {\n"
                                 "  int* p = std::find_if(a, a + n, is_zero);\n"
                                 "  if (p == a + n) return 0;\n"
                                 "  return *p;\n"
                                 "}\n";
    std::string find_if_fn_err;
    CHECK(syntax_only(c.cxx, find_if_fn, false, &find_if_fn_err));
    CHECK(syntax_only(c.cxx, find_if_fn, true, &find_if_fn_err));

    const fs::path find_if_val = c.tmp / "find_if_val.cpp";
    std::ofstream(find_if_val) << "#include <algorithm>\n"
                                  "bool is_zero(int x) { return x == 0; }\n"
                                  "int use(int* a, int n) {\n"
                                  "  return *std::find_if(a, a + n, is_zero);\n"
                                  "}\n";
    std::string find_if_val_err;
    CHECK(syntax_only(c.cxx, find_if_val, false, &find_if_val_err));
    CHECK_FALSE(syntax_only(c.cxx, find_if_val, true, &find_if_val_err));

    const fs::path count_ok = c.tmp / "count_ok.cpp";
    std::ofstream(count_ok) << "#include <algorithm>\n"
                               "#include <cstddef>\n"
                               "std::ptrdiff_t use(int* a, int n, const int& v) {\n"
                               "  return std::count(a, a + n, v);\n"
                               "}\n";
    std::string count_ok_err;
    CHECK(syntax_only(c.cxx, count_ok, false, &count_ok_err));
    CHECK(syntax_only(c.cxx, count_ok, true, &count_ok_err));

    const fs::path count_long = c.tmp / "count_long.cpp";
    std::ofstream(count_long) << "#include <algorithm>\n"
                                 "long use(int* a, int n) {\n"
                                 "  return std::count(a, a + n, 0L);\n"
                                 "}\n";
    std::string count_long_err;
    CHECK(syntax_only(c.cxx, count_long, false, &count_long_err));
    CHECK_FALSE(syntax_only(c.cxx, count_long, true, &count_long_err));

    const fs::path count_const = c.tmp / "count_const.cpp";
    std::ofstream(count_const) << "#include <algorithm>\n"
                                  "int use(const int* a, int n, int v) {\n"
                                  "  return static_cast<int>(std::count(a, a + n, v));\n"
                                  "}\n";
    std::string count_const_err;
    CHECK(syntax_only(c.cxx, count_const, false, &count_const_err));
    CHECK_FALSE(syntax_only(c.cxx, count_const, true, &count_const_err));

    const fs::path count_if = c.tmp / "count_if.cpp";
    std::ofstream(count_if) << "#include <algorithm>\n"
                               "int use(int* a, int n) {\n"
                               "  return static_cast<int>(std::count_if(a, a + n, [](int x) { return x == 0; }));\n"
                               "}\n";
    std::string count_if_err;
    CHECK(syntax_only(c.cxx, count_if, false, &count_if_err));
    CHECK_FALSE(syntax_only(c.cxx, count_if, true, &count_if_err));

    const fs::path count_empty_ok = c.tmp / "count_empty_ok.cpp";
    std::ofstream(count_empty_ok) << "#include <algorithm>\n"
                                     "constexpr int empty() {\n"
                                     "  int a[] = {1, 2, 3};\n"
                                     "  return static_cast<int>(std::count(a, a, 1));\n"
                                     "}\n"
                                     "static_assert(empty() == 0);\n";
    std::string count_empty_ok_err;
    CHECK(syntax_only(c.cxx, count_empty_ok, false, &count_empty_ok_err));
    CHECK(syntax_only(c.cxx, count_empty_ok, true, &count_empty_ok_err));

    const fs::path inverted_find_if = c.tmp / "invert_find_if_const.cpp";
    std::ofstream(inverted_find_if) << "#include <algorithm>\n"
                                       "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                       "constexpr int inverted() {\n"
                                       "  int a[] = {1, 2, 3};\n"
                                       "  return *std::find_if(a + 2, a, is_one);\n"
                                       "}\n"
                                       "static_assert(inverted() == 0);\n";
    std::string inverted_find_if_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_find_if, true, &inverted_find_if_err));

    const fs::path null_find_if = c.tmp / "null_find_if_const.cpp";
    std::ofstream(null_find_if) << "#include <algorithm>\n"
                                   "constexpr int nullp() {\n"
                                   "  int a[] = {1, 2, 3};\n"
                                   "  return *std::find_if(a, a + 3, static_cast<bool (*)(const int&)>(nullptr));\n"
                                   "}\n"
                                   "static_assert(nullp() == 0);\n";
    std::string null_find_if_err;
    CHECK_FALSE(syntax_only(c.cxx, null_find_if, true, &null_find_if_err));

    const fs::path inverted_count = c.tmp / "invert_count_const.cpp";
    std::ofstream(inverted_count) << "#include <algorithm>\n"
                                     "constexpr int inverted() {\n"
                                     "  int a[] = {1, 2, 3};\n"
                                     "  return static_cast<int>(std::count(a + 2, a, 1));\n"
                                     "}\n"
                                     "static_assert(inverted() == 0);\n";
    std::string inverted_count_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_count, true, &inverted_count_err));
}
