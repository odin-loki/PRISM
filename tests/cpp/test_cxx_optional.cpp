// Differential test of the std::optional model header
// (src/prism/pir/models/cxx/optional). Same shape as tests/cpp/test_cxx_list.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/optional.md has the line that links this file.
//
// An engaged optional yields the stored T. value() / operator* / operator-> on
// an empty optional are a __prism_check, and the model does not print a fake T.
// value_or(99) is the stored T when engaged, and 99 when disengaged.
// and_then calls the function with the stored T when engaged and returns that
// result. A disengaged optional does not call the function and yields a
// disengaged optional.
// or_else calls the function when disengaged and returns that optional. An
// engaged optional does not call the function and yields an optional that
// holds the stored T.

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

fs::path build_value_or(const std::string& cxx, const fs::path& tmp, const fs::path& src, bool with_models,
                        const std::string& stem = "value_or_trace") {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (stem + (with_models ? ".model" : ".lib"));
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

TEST_CASE("cxx optional model: and_then calls the function or returns disengaged") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = repo() / "tests" / "cxx_models" / "optional_and_then_trace.cpp";
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

    auto lib = run_lines(build_value_or(c.cxx, c.tmp, src, false, "and_then_trace"));
    auto mod = run_lines(build_value_or(c.cxx, c.tmp, src, true, "and_then_trace"));
    const std::vector<std::string> want{
        "engaged 1 43", "empty 0",        "calls 1", "make_empty 42", "cleared 0", "still 0", "calls2 1",
        "held 1 42",    "none 0",         "zero 1 0", "zeroE 0",      "chain 1 44", "chainE 0", "calls3 2",
        "cat-lv",       "lv 1",           "cat-clv", "clv 3",         "cat-rv",    "rv 2",     "cat-crv",
        "crv 4",        "elv 0",          "eclv 0",  "erv 0",         "ecrv 0",    "box 1 43", "boxE 0",
    };
    CHECK(lib == mod);
    CHECK(mod == want);
    for (const auto& line : mod) {
        CHECK(line.find("bad") == std::string::npos);
        CHECK(line != "engaged 1 42");
        CHECK(line != "engaged 1 0");
        CHECK(line != "engaged 0 0");
        CHECK(line != "empty 1");
        CHECK(line != "calls 0");
        CHECK(line != "calls 2");
        CHECK(line != "calls2 0");
        CHECK(line != "calls2 2");
        CHECK(line != "calls3 0");
        CHECK(line != "calls3 1");
        CHECK(line != "calls3 4");
        CHECK(line != "cleared 1");
        CHECK(line != "still 1");
        CHECK(line != "held 1 0");
        CHECK(line != "held 0 0");
        CHECK(line != "none 1");
        CHECK(line != "zero 0 0");
        CHECK(line != "zero 1 42");
        CHECK(line != "zeroE 1");
        CHECK(line != "chain 1 42");
        CHECK(line != "chain 1 0");
        CHECK(line != "chainE 1");
        CHECK(line != "box 1 42");
        CHECK(line != "box 1 0");
        CHECK(line != "boxE 1");
        CHECK(line != "elv 1");
        CHECK(line != "eclv 1");
        CHECK(line != "erv 1");
        CHECK(line != "ecrv 1");
    }
}

TEST_CASE("cxx optional model: or_else calls the function or keeps the stored T") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = repo() / "tests" / "cxx_models" / "optional_or_else_trace.cpp";
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

    auto lib = run_lines(build_value_or(c.cxx, c.tmp, src, false, "or_else_trace"));
    auto mod = run_lines(build_value_or(c.cxx, c.tmp, src, true, "or_else_trace"));
    const std::vector<std::string> want{
        "engaged 1 42", "empty 1 7",     "calls 1",     "zero 1 0",  "gotzero",   "zeroE 1 0", "calls2 0",
        "kept 1 42",    "make_empty",    "cleared 0",   "calls3 1",  "chain 1 7", "chainE 1 42", "calls4 1",
        "call-lv",      "lv 1",          "call-clv",    "clv 3",     "call-rv",   "rv 2",      "call-crv",
        "crv 4",        "keep-lv 42",    "keep-clv 42", "keep-rv 42", "keep-crv 42", "box 1 42 42", "makebox",
        "boxE 1 9",     "boxnone 0",     "moved 42 -1", "cmove 5 5",
    };
    CHECK(lib == mod);
    CHECK(mod == want);
    for (const auto& line : mod) {
        CHECK(line.find("bad") == std::string::npos);
        CHECK(line != "second");
        CHECK(line != "engaged 1 7");
        CHECK(line != "engaged 1 0");
        CHECK(line != "engaged 0 0");
        CHECK(line != "empty 0");
        CHECK(line != "empty 1 0");
        CHECK(line != "empty 1 42");
        CHECK(line != "calls 0");
        CHECK(line != "calls 2");
        CHECK(line != "zero 1 7");
        CHECK(line != "zero 0 0");
        CHECK(line != "zeroE 0");
        CHECK(line != "calls2 1");
        CHECK(line != "kept 0 0");
        CHECK(line != "kept 1 0");
        CHECK(line != "cleared 1");
        CHECK(line != "calls3 0");
        CHECK(line != "calls3 2");
        CHECK(line != "chain 1 8");
        CHECK(line != "chain 1 0");
        CHECK(line != "chainE 1 7");
        CHECK(line != "chainE 1 0");
        CHECK(line != "calls4 0");
        CHECK(line != "calls4 2");
        CHECK(line != "keep-lv 1");
        CHECK(line != "keep-clv 3");
        CHECK(line != "keep-rv 2");
        CHECK(line != "keep-crv 4");
        CHECK(line != "box 1 0 42");
        CHECK(line != "box 1 42 -1");
        CHECK(line != "boxE 1 0");
        CHECK(line != "boxnone 1");
        CHECK(line != "moved 42 42");
        CHECK(line != "moved 0 -1");
        CHECK(line != "cmove 5 -1");
        CHECK(line != "cmove 0 5");
    }
}
