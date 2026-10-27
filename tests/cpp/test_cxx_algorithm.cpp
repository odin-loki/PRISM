// Differential test of the partial <algorithm> model
// (src/prism/pir/models/cxx/algorithm). The header is a file, so
// `#include <algorithm>` is not EISDIR. CMake's prism_tests source list is
// explicit (not a glob).
//
// min and max return one of the two arguments. Equal arguments return the
// first. An inverted clamp is __prism_check then __builtin_trap, and that
// path does not return v. min of the extreme ints is not 0. sort, find,
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
    if (out) *out = r.out;
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
    CHECK(text.find("return 0") == std::string::npos);
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
    for (const char* name : {"sort(", "find(", "copy(", "fill(", "for_each("}) CHECK(text.find(name) == std::string::npos);

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

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "invert"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("clamp: hi < lo") != std::string::npos);
    CHECK(out.find("value=") == std::string::npos);

    const fs::path omitted = c.tmp / "omitted.cpp";
    std::ofstream(omitted) << "#include <algorithm>\n"
                              "int use(int* a, int n) {\n"
                              "  std::sort(a, a + n);\n"
                              "  std::find(a, a + n, 0);\n"
                              "  std::copy(a, a + n, a);\n"
                              "  std::fill(a, a + n, 0);\n"
                              "  std::for_each(a, a + n, [](int) {});\n"
                              "  return a[0];\n"
                              "}\n";
    std::string omitted_err;
    CHECK(syntax_only(c.cxx, omitted, false, &omitted_err));
    CHECK_FALSE(syntax_only(c.cxx, omitted, true, &omitted_err));

    const fs::path inverted = c.tmp / "invert_const.cpp";
    std::ofstream(inverted) << "#include <algorithm>\n"
                               "static_assert(std::clamp(0, 2, 1) == 0);\n";
    std::string inverted_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted, true, &inverted_err));
}
