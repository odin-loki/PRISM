// std::counting_semaphore model (src/prism/pir/models/cxx/semaphore).
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/semaphore.md has the line that links this file.
//
// release past the max template argument is a __prism_check and does not
// wrap the counter. try_acquire on zero is false and does not decrement, so
// a later release still adds a token. The header does not define a blocking
// acquire that returns success.

#include <doctest/doctest.h>

#include <cctype>
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

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#ifdef _WIN32
constexpr const char* kProbeExe = "probe.exe";
#else
constexpr const char* kProbeExe = "probe";
#endif

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
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <semaphore>\n"
                            "extern \"C\" void __prism_check(int, const char*, const char*) {}\n"
                            "int main() {\n"
                            "  std::counting_semaphore<1> s(0);\n"
                            "  return s.max() == 1 ? 0 : 1;\n"
                            "}\n";
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> build{cxx};
    build.insert(build.end(), kFlags.begin(), kFlags.end());
    build.push_back("-isystem");
    build.push_back(models.string());
    build.push_back(probe.string());
    build.push_back("-o");
    build.push_back((tmp / kProbeExe).string());
    if (!run_cmd(build)) return false;
    return run_cmd({(tmp / kProbeExe).string()});
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    fs::path out = tmp / (with_models ? "semaphore_trace.model" : "semaphore_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), kFlags.begin(), kFlags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "semaphore_trace.cpp").string());
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
        for (const char* name : {"clang++-18", "clang++"}) {
            if (run_cmd({name, "--version"})) {
                c.cxx = name;
                break;
            }
        }
        if (!c.cxx.empty()) c.ok = probe_clang_asan(c.cxx, c.tmp);
    }
    if (!c.ok) MESSAGE("NOTRUN: clang++ with ASan/UBSan and the semaphore model not usable here");
}

TEST_CASE("cxx semaphore model: zero try_acquire is false and release past max is a check") {
    auto& c = ctx();
    if (!c.ok) return;
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
        MESSAGE("libstdc++ semaphore trace did not link; model trace still checked");
    }
}
