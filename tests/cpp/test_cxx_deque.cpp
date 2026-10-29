// Differential test of the std::deque model header. Standalone: it does not
// share fixtures with tests/cpp/test_cxx_models.cpp. The header is on the
// pir include path (embedded as cxx/deque); this test compiles
// tests/cxx_models/deque_trace.cpp twice, once against libstdc++ and once with
// -isystem on a directory holding only the model, so #include <deque> is it.
//
// Without a libstdc++ clang++ that can run ASan/UBSan the cases report NOTRUN.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <deque>\n"
                            "int main() { std::deque<int> d{1}; return d[0] - 1; }\n";
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

const std::vector<std::string>& flags() {
    static const std::vector<std::string> f{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                             "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    return f;
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    fs::path out = tmp / (with_models ? "deque_trace.model" : "deque_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags().begin(), flags().end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(model_dir().string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "deque_trace.cpp").string());
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

TEST_CASE("cxx deque: setup clang++ with ASan/UBSan and libstdc++") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxdeque-doctest";
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
    if (!c.ok) MESSAGE("NOTRUN: clang++ with libstdc++ and ASan/UBSan not usable here");
}

TEST_CASE("cxx deque: trace matches libstdc++") {
    auto& c = ctx();
    if (!c.ok) return;
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
    if (!c.ok) return;
    // libstdc++ does not free a block on this push, so ASan stays quiet there.
    // The model deletes the iterator anchor; the load in operator* is the UAF.
    auto mod = c.tmp / "deque_trace.model";
    if (!fs::exists(mod)) mod = build_prog(c.cxx, c.tmp, true);
    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "uaf"}, &out, &err));
    CHECK((out + err).find("heap-use-after-free") != std::string::npos);
}
