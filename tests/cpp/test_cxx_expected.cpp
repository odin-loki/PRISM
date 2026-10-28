// Differential test of the std::expected model header
// (src/prism/pir/models/cxx/expected). Same shape as tests/cpp/test_cxx_list.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/expected.md has the line that links this file.
//
// A success value() is the stored T. value() / operator* on an unexpected are
// a __prism_check, and the model does not print a fake T. error() on a success
// value is a __prism_check, and the model does not print a fake E.
// and_then on a value returns the function's expected; on an unexpected it
// does not call the function and keeps that E. or_else on an unexpected
// returns the function's expected; on a value it does not call the function
// and keeps that T. Neither path invents a default T or a default E.
// value_or on a value returns that T. value_or on an unexpected returns the
// argument converted to T, not a default T and not the stored E.

#include <doctest/doctest.h>

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

struct ExpCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

ExpCtx& ctx() {
    static ExpCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <expected>\n"
                            "int main() { std::expected<int, int> e(1); return e.value() - 1; }\n";
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
    fs::path out = tmp / (with_models ? "expected_trace.model" : "expected_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "expected_trace.cpp").string());
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

bool failed_is_check(const fs::path& exe, const char* mode, const char* cls, const char* which) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    const std::string needle = std::string("CHECK ") + cls + ":";
    if (both.find(needle) == std::string::npos) return false;
    if (both.find(which) == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx expected model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxexpected-doctest";
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

TEST_CASE("cxx expected model: success value() is the stored T; value() and error() checks") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "expected_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "expected").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 5);
    CHECK(a == b);
    bool saw_stored = false;
    bool saw_error = false;
    bool saw_and_then = false;
    bool saw_and_then_unex = false;
    bool saw_or_else = false;
    bool saw_or_else_unex = false;
    bool saw_and_then_not = false;
    bool saw_or_else_not = false;
    bool saw_got = false;
    bool saw_and_then_val = false;
    bool saw_or_else_e = false;
    bool saw_and_then_r = false;
    bool saw_and_then_const = false;
    bool saw_or_else_const = false;
    bool saw_and_then_rv = false;
    bool saw_or_else_rv = false;
    bool saw_and_then_rv_unex = false;
    bool saw_or_else_rv_val = false;
    bool saw_and_then_cmv = false;
    bool saw_or_else_cmv = false;
    bool saw_value_or = false;
    bool saw_value_or_unex = false;
    bool saw_value_or_rv = false;
    bool saw_value_or_rv_unex = false;
    bool saw_value_or_zero = false;
    bool saw_value_or_const = false;
    bool saw_value_or_const_unex = false;
    bool saw_value_or_held = false;
    bool saw_value_or_none = false;
    for (const auto& line : b) {
        if (line == "engaged 1 1 42 42") saw_stored = true;
        if (line == "unex 0 0 7") saw_error = true;
        if (line == "and_then 1 84") saw_and_then = true;
        if (line == "and_then_unex 0 0 9") saw_and_then_unex = true;
        if (line == "or_else 0 1 42") saw_or_else = true;
        if (line == "or_else_unex 1 11") saw_or_else_unex = true;
        if (line == "and_then_not 0 4") saw_and_then_not = true;
        if (line == "or_else_not 1 42") saw_or_else_not = true;
        if (line == "got 6") saw_got = true;
        if (line == "and_then_val 6") saw_and_then_val = true;
        if (line == "or_else_e 1 2") saw_or_else_e = true;
        if (line == "and_then_r 0 3") saw_and_then_r = true;
        if (line == "and_then_const 45") saw_and_then_const = true;
        if (line == "or_else_const 10") saw_or_else_const = true;
        if (line == "and_then_rv 15") saw_and_then_rv = true;
        if (line == "or_else_rv 7") saw_or_else_rv = true;
        if (line == "and_then_rv_unex 0 6") saw_and_then_rv_unex = true;
        if (line == "or_else_rv_val 1 8") saw_or_else_rv_val = true;
        if (line == "and_then_cmv 0 4") saw_and_then_cmv = true;
        if (line == "or_else_cmv 1 8") saw_or_else_cmv = true;
        if (line == "value_or 42") saw_value_or = true;
        if (line == "value_or_unex 99") saw_value_or_unex = true;
        if (line == "value_or_rv 42") saw_value_or_rv = true;
        if (line == "value_or_rv_unex 99") saw_value_or_rv_unex = true;
        if (line == "value_or_zero 0") saw_value_or_zero = true;
        if (line == "value_or_const 42") saw_value_or_const = true;
        if (line == "value_or_const_unex 99") saw_value_or_const_unex = true;
        if (line == "value_or_held 42") saw_value_or_held = true;
        if (line == "value_or_none 99") saw_value_or_none = true;
        CHECK(line.find("engaged 1 1 0 0") == std::string::npos);
        CHECK(line != "held 0");
        CHECK(line != "copy 0");
        CHECK(line != "err 0");
        CHECK(line != "unex 0 0 0");
        CHECK(line != "and_then 1 0");
        CHECK(line != "and_then_val 0");
        CHECK(line != "or_else 0 1 0");
        CHECK(line != "or_else_unex 1 0");
        CHECK(line != "and_then_unex 0 0 0");
        CHECK(line != "or_else_e 1 0");
        CHECK(line != "and_then_r 0 0");
        CHECK(line.find("callT") == std::string::npos);
        CHECK(line.find("callR") == std::string::npos);
        CHECK(line.find("callEo") == std::string::npos);
        CHECK(line.find("callRu") == std::string::npos);
        CHECK(line.find("callRv") == std::string::npos);
        CHECK(line.find("callOr") == std::string::npos);
        CHECK(line.find("callCmv") == std::string::npos);
        CHECK(line.find("callCOr") == std::string::npos);
        CHECK(line != "value_or 99");
        CHECK(line != "value_or 0");
        CHECK(line != "value_or_unex 0");
        CHECK(line != "value_or_unex 9");
        CHECK(line != "value_or_rv 99");
        CHECK(line != "value_or_rv 0");
        CHECK(line != "value_or_rv_unex 0");
        CHECK(line != "value_or_rv_unex 7");
        CHECK(line != "value_or_zero 99");
        CHECK(line != "value_or_const 99");
        CHECK(line != "value_or_const 0");
        CHECK(line != "value_or_const_unex 0");
        CHECK(line != "value_or_const_unex 3");
        CHECK(line != "value_or_held 99");
        CHECK(line != "value_or_held 0");
        CHECK(line != "value_or_none 0");
        CHECK(line != "value_or_none 4");
    }
    CHECK(saw_stored);
    CHECK(saw_error);
    CHECK(saw_and_then);
    CHECK(saw_and_then_unex);
    CHECK(saw_or_else);
    CHECK(saw_or_else_unex);
    CHECK(saw_and_then_not);
    CHECK(saw_or_else_not);
    CHECK(saw_got);
    CHECK(saw_and_then_val);
    CHECK(saw_or_else_e);
    CHECK(saw_and_then_r);
    CHECK(saw_and_then_const);
    CHECK(saw_or_else_const);
    CHECK(saw_and_then_rv);
    CHECK(saw_or_else_rv);
    CHECK(saw_and_then_rv_unex);
    CHECK(saw_or_else_rv_val);
    CHECK(saw_and_then_cmv);
    CHECK(saw_or_else_cmv);
    CHECK(saw_value_or);
    CHECK(saw_value_or_unex);
    CHECK(saw_value_or_rv);
    CHECK(saw_value_or_rv_unex);
    CHECK(saw_value_or_zero);
    CHECK(saw_value_or_const);
    CHECK(saw_value_or_const_unex);
    CHECK(saw_value_or_held);
    CHECK(saw_value_or_none);

    CHECK(failed_is_check(mod, "value", "CXX-EXPECTED-NULL", "expected::value()"));
    CHECK(failed_is_check(mod, "star", "CXX-EXPECTED-NULL", "expected::operator*"));
    CHECK(failed_is_check(mod, "error", "CXX-EXPECTED-ERROR", "expected::error()"));
}
