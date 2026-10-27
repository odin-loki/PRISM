// Differential test of the std::optional model header
// (src/prism/pir/models/cxx/optional). Same shape as tests/cpp/test_cxx_list.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/optional.md has the line that links this file.
//
// An engaged optional yields the stored T. value() / operator* / operator-> on
// an empty optional are a __prism_check, and the model does not print a fake T.
// value_or(99) is the stored T when engaged, and 99 when disengaged.

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

struct OptCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

OptCtx& ctx() {
    static OptCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <optional>\n"
                            "int main() { std::optional<int> o(1); return o.value() - 1; }\n";
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
    fs::path out = tmp / (with_models ? "optional_trace.model" : "optional_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "optional_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

fs::path build_value_or(const std::string& cxx, const fs::path& tmp, const fs::path& src, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (with_models ? "value_or_trace.model" : "value_or_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
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

bool disengaged_is_check(const fs::path& exe, const char* mode, const char* which) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK CXX-OPTIONAL-NULL:") == std::string::npos) return false;
    if (both.find(which) == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx optional model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxoptional-doctest";
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

TEST_CASE("cxx optional model: engaged value is the stored T and empty value() is a check") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "optional_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "optional").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 5);
    CHECK(a == b);
    bool saw_stored = false;
    for (const auto& line : b) {
        if (line == "engaged 1 1 42 42") saw_stored = true;
        CHECK(line.find("engaged 1 1 0 0") == std::string::npos);
        CHECK(line != "held 0");
        CHECK(line != "copy 0");
    }
    CHECK(saw_stored);

    CHECK(disengaged_is_check(mod, "empty", "optional::value()"));
    CHECK(disengaged_is_check(mod, "star", "optional::operator*"));
    CHECK(disengaged_is_check(mod, "arrow", "optional::operator->"));
}

TEST_CASE("cxx optional model: value_or returns the stored T or the argument") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = c.tmp / "optional_value_or.cpp";
    // Box has no default constructor. value_or(99) has to convert 99; it
    // cannot value-initialize a Box. The stored value is 42, not the fallback.
    std::ofstream(src) << "#include <cstdio>\n"
                          "#include <optional>\n"
                          "struct Box {\n"
                          "    int v;\n"
                          "    Box() = delete;\n"
                          "    Box(int x) : v(x) {}\n"
                          "};\n"
                          "int main() {\n"
                          "    std::optional<int> engaged(42);\n"
                          "    std::optional<int> empty;\n"
                          "    std::printf(\"engaged %d\\n\", engaged.value_or(99));\n"
                          "    std::printf(\"empty %d\\n\", empty.value_or(99));\n"
                          "    std::printf(\"moved %d\\n\", std::optional<int>(42).value_or(99));\n"
                          "    std::printf(\"movedE %d\\n\", std::optional<int>().value_or(99));\n"
                          "    std::optional<Box> held(Box(42));\n"
                          "    std::optional<Box> none;\n"
                          "    std::printf(\"held %d\\n\", held.value_or(99).v);\n"
                          "    std::printf(\"none %d\\n\", none.value_or(99).v);\n"
                          "    return 0;\n"
                          "}\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back(src.string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "optional").string())) != std::string::npos);

    auto lib = run_lines(build_value_or(c.cxx, c.tmp, src, false));
    auto mod = run_lines(build_value_or(c.cxx, c.tmp, src, true));
    CHECK(lib == mod);
    REQUIRE(mod.size() == 6);
    CHECK(mod[0] == "engaged 42");
    CHECK(mod[1] == "empty 99");
    CHECK(mod[2] == "moved 42");
    CHECK(mod[3] == "movedE 99");
    CHECK(mod[4] == "held 42");
    CHECK(mod[5] == "none 99");
    for (const auto& line : mod) {
        CHECK(line != "engaged 99");
        CHECK(line != "engaged 0");
        CHECK(line != "empty 0");
        CHECK(line != "moved 99");
        CHECK(line != "movedE 0");
        CHECK(line != "held 99");
        CHECK(line != "held 0");
        CHECK(line != "none 0");
    }
}
