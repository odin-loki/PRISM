// Shared driver for the C++ standard-library model tests (cxx_model_harness.hpp).
#include "cxx_model_harness.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  include "support/proctree.hpp"
#endif

namespace prism_test::cxx {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_dir() { return repo() / "src" / "prism" / "pir" / "models" / "cxx"; }

fs::path trace_src(std::string_view file) { return repo() / "tests" / "cxx_models" / fs::path(std::string(file)); }

const std::vector<std::string>& asan_flags() {
    static const std::vector<std::string> f{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                            "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    return f;
}

bool run_cmd(const std::vector<std::string>& argv, std::string* out, std::string* err) {
    if (argv.empty()) return false;
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
    // Same runner as replay.cpp's sanitizers_usable(): no shell, no posix_spawn.
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

const std::vector<std::string>& cxx_candidates() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v;
#ifdef _WIN32
        constexpr const char* kName = "clang++.exe";
#else
        constexpr const char* kName = "clang++";
#endif
        // The pinned LLVM (CI fetches it and sets PRISM_LLVM_BIN), so the models
        // are tested with the compiler PRISM itself pins, not whatever is on PATH.
        std::string discard;
        if (const char* bin = std::getenv("PRISM_LLVM_BIN"); bin && *bin) {
            const std::string pinned = (fs::path(bin) / kName).string();
            if (run_cmd({pinned, "--version"}, &discard, nullptr)) v.push_back(pinned);
        }
        if (run_cmd({"clang++", "--version"}, &discard, nullptr)) v.emplace_back("clang++");
        return v;
    }();
    return list;
}

std::string pick_cxx(const std::function<bool(const std::string&)>& probe) {
    for (const auto& cxx : cxx_candidates())
        if (probe(cxx)) return cxx;
    return {};
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp, std::string_view program,
                      const std::vector<std::string>& flags, const std::vector<std::string>& after) {
#ifdef _WIN32
    constexpr const char* kProbeExe = "probe.exe";
#else
    constexpr const char* kProbeExe = "probe";
#endif
    const auto probe = tmp / "probe.cpp";
    {
        std::ofstream o(probe);
        // libstdc++ only: see the header comment.
        o << "#include <bits/c++config.h>\n" << program;
    }
    std::vector<std::string> build{cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.insert(build.end(), after.begin(), after.end());
    build.push_back("-o");
    build.push_back((tmp / kProbeExe).string());
    if (!run_cmd(build)) return false;
    // Run it once too: on Windows a clang that links the dynamic ASan runtime needs its
    // DLL on PATH, and a missing one only shows when the binary starts.
    return run_cmd({(tmp / kProbeExe).string()});
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, const fs::path& src, const std::string& stem,
                    const std::vector<std::string>& flags, bool with_models) {
    fs::path out = tmp / (stem + (with_models ? ".model" : ".lib"));
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(model_dir().string());
    }
    argv.push_back(src.string());
    argv.push_back("-o");
    argv.push_back(out.string());
    INFO("build: " << src.string());
    REQUIRE(run_cmd(argv));
    return out;
}

std::vector<std::string> run_lines(const fs::path& exe, const std::vector<std::string>& args) {
    std::vector<std::string> argv{exe.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string stdout_text;
    INFO("run: " << exe.string());
    REQUIRE(run_cmd(argv, &stdout_text));
    std::vector<std::string> lines;
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

fs::path fresh_tmp(std::string_view name) {
    const fs::path p = fs::temp_directory_path() / fs::path(std::string(name));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

}  // namespace prism_test::cxx
