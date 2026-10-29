// Differential test of the integer std::from_chars / std::to_chars model
// (src/prism/pir/models/cxx/charconv). Same shape as tests/cpp/test_cxx_list.cpp:
// this driver is its own translation unit. CMake's prism_tests source list is
// explicit (not a glob). The trace is tests/cxx_models/charconv_trace.cpp.
//
// Without a libstdc++ clang++ that can run ASan/UBSan the cases report NOTRUN.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

struct CharconvCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
    bool built = false;
    fs::path model_exe;
    std::vector<std::string> lib_lines;
    std::vector<std::string> model_lines;
};

CharconvCtx& ctx() {
    static CharconvCtx c;
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
    // environ comes from <unistd.h> (a block-scope extern here would get internal linkage).
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <charconv>\n"
                            "int main() {\n"
                            "  int v = 0;\n"
                            "  const char* s = \"7\";\n"
                            "  auto r = std::from_chars(s, s + 1, v);\n"
                            "  return v - 7 + (r.ec == std::errc{} ? 0 : 1);\n"
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
    fs::path out = tmp / (with_models ? "charconv_trace.model" : "charconv_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "charconv_trace.cpp").string());
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

bool has_line(const std::vector<std::string>& lines, const char* exact) {
    return std::find(lines.begin(), lines.end(), exact) != lines.end();
}

void ensure_built() {
    auto& c = ctx();
    if (!c.ok || c.built) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "charconv_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    REQUIRE(fwd(deps).find(fwd((models / "charconv").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    c.model_exe = build_prog(c.cxx, c.tmp, true);
    c.lib_lines = run_lines(lib);
    c.model_lines = run_lines(c.model_exe);
    c.built = true;
}

// A failing __prism_check aborts. The stand-in prints CHECK <class>: <message>
// and must not reach the "fake success" printf.
void failed_is_check(const fs::path& exe, const char* arg, const char* cls, const char* which) {
    std::string out;
    std::string err;
    CHECK_FALSE(run_cmd({exe.string(), arg}, &out, &err));
    CHECK(out.find("fake=") == std::string::npos);
    const std::string needle = std::string("CHECK ") + cls + ":";
    CHECK(err.find(needle) != std::string::npos);
    CHECK(err.find(which) != std::string::npos);
}

}  // namespace

TEST_CASE("cxx charconv model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxcharconv-doctest";
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

TEST_CASE("cxx charconv model: a small number stores that number") {
    auto& c = ctx();
    if (!c.ok) return;
    ensure_built();
    CHECK(c.lib_lines == c.model_lines);
    CHECK(has_line(c.model_lines, "small ec=1 v=42 off=2"));
}

TEST_CASE("cxx charconv model: overflow does not store a success value") {
    auto& c = ctx();
    if (!c.ok) return;
    ensure_built();
    CHECK(c.lib_lines == c.model_lines);
    // Sentinels stay put: 12345 is not a wrapped int, 77 is not a zero from
    // 2^32, and ec=2 is result_out_of_range rather than success.
    CHECK(has_line(c.model_lines, "over ec=2 v=12345 off=10"));
    CHECK(has_line(c.model_lines, "over0 ec=2 v=77 off=10"));
    CHECK(has_line(c.model_lines, "uover ec=2 v=5 off=10"));
    CHECK(has_line(c.model_lines, "minover ec=2 v=3 off=11"));
    // A buffer of two bytes cannot hold 100000. ec=4 is value_too_large,
    // success=0, and the canary bytes are unchanged (nothing written past last).
    CHECK(has_line(c.model_lines, "short ec=4 success=0 b0=Q b1=Q z2=Z z3=Z"));
}

TEST_CASE("cxx charconv model: a null range is a check") {
    auto& c = ctx();
    if (!c.ok) return;
    ensure_built();
    failed_is_check(c.model_exe, "null", "PTR-NULL-DEREF", "from_chars");
    failed_is_check(c.model_exe, "invert", "FUNC-CONTRACT", "last before first");
}
