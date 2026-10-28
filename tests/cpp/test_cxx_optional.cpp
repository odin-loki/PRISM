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
// transform calls the function with the stored T when engaged and yields an
// optional that holds what the function returns. A disengaged optional does
// not call the function and yields a disengaged optional, not T{}.
// operator== and operator< compare two optional<T>. Two disengaged optionals
// compare equal and neither is less. A disengaged optional is less than an
// engaged one. Two engaged optionals compare the stored values. The inactive
// T is not read and T{} is not built. That comparison is not PROVED and not
// CLEAN.

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

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
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
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// The function whose signature starts at sig. The next character after the
// signature in this header is '{'.
std::string function_body(const std::string& src, std::size_t sig) {
    const auto brace = src.find('{', sig);
    if (brace == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    return {};
}

int count_of(const std::string& s, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos; at += needle.size()) ++n;
    return n;
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

TEST_CASE("cxx optional model: transform calls the function or returns disengaged") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = repo() / "tests" / "cxx_models" / "optional_transform_trace.cpp";
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

    auto lib = run_lines(build_value_or(c.cxx, c.tmp, src, false, "transform_trace"));
    auto mod = run_lines(build_value_or(c.cxx, c.tmp, src, true, "transform_trace"));
    const std::vector<std::string> want{
        "engaged 1 43", "empty 0",    "calls 1",  "zeroIn 1 1", "calls2 1", "zero 1 0",    "zeroE 0",
        "nullp 1 1",    "nullpE 0",   "to_box 42", "held 1 42", "none 0",   "cv 43",       "cvE 0",
        "wrap 1 1 43",  "wrapE 0",    "chain 1 44", "chainE 0", "calls3 2", "cat-lv",      "lv 1",
        "cat-clv",      "clv 3",      "cat-rv",    "rv 2",      "cat-crv",  "crv 4",       "elv 0",
        "eclv 0",       "erv 0",      "ecrv 0",    "box 1 43 42", "boxE 0", "moved 42 -1", "cmove 5 5",
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
        CHECK(line != "zeroIn 1 0");
        CHECK(line != "zeroIn 0 0");
        CHECK(line != "calls2 0");
        CHECK(line != "zero 1 42");
        CHECK(line != "zero 0 0");
        CHECK(line != "zeroE 1");
        CHECK(line != "nullp 1 0");
        CHECK(line != "nullp 0 1");
        CHECK(line != "nullpE 1");
        CHECK(line != "to_box 0");
        CHECK(line != "held 1 0");
        CHECK(line != "held 0 0");
        CHECK(line != "none 1");
        CHECK(line != "cv 42");
        CHECK(line != "cv 0");
        CHECK(line != "cvE 1");
        CHECK(line != "wrap 1 1 42");
        CHECK(line != "wrap 1 0 0");
        CHECK(line != "wrapE 1");
        CHECK(line != "chain 1 42");
        CHECK(line != "chain 1 43");
        CHECK(line != "chain 1 0");
        CHECK(line != "chainE 1");
        CHECK(line != "calls3 0");
        CHECK(line != "calls3 1");
        CHECK(line != "calls3 4");
        CHECK(line != "lv 42");
        CHECK(line != "clv 42");
        CHECK(line != "rv 42");
        CHECK(line != "crv 42");
        CHECK(line != "elv 1");
        CHECK(line != "eclv 1");
        CHECK(line != "erv 1");
        CHECK(line != "ecrv 1");
        CHECK(line != "box 1 42 42");
        CHECK(line != "box 1 43 -1");
        CHECK(line != "box 1 0 42");
        CHECK(line != "boxE 1");
        CHECK(line != "moved 42 42");
        CHECK(line != "moved -1 -1");
        CHECK(line != "moved 0 -1");
        CHECK(line != "cmove 5 -1");
        CHECK(line != "cmove -1 5");
    }
}

TEST_CASE("cxx optional model: compare is the stored value only when both are engaged") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "optional";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <functional>") == std::string::npos);
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 3);

    const auto cls = src.find("class optional {");
    const auto kind = src.find("struct _Prism_optional_kind<optional<_T>>");
    const auto eq = src.find("bool operator==(const optional<_Tp>& __x, const optional<_Tp>& __y) {");
    const auto lt = src.find("bool operator<(const optional<_Tp>& __x, const optional<_Tp>& __y) {");
    REQUIRE(cls != std::string::npos);
    REQUIRE(kind != std::string::npos);
    REQUIRE(eq != std::string::npos);
    REQUIRE(lt != std::string::npos);
    CHECK(cls < kind);
    CHECK(kind < eq);
    CHECK(eq < lt);

    const std::string eq_body = function_body(src, eq);
    const std::string lt_body = function_body(src, lt);
    REQUIRE_FALSE(eq_body.empty());
    REQUIRE_FALSE(lt_body.empty());
    CHECK(eq_body.find("if (__x._M_engaged != __y._M_engaged) return false;") != std::string::npos);
    CHECK(eq_body.find("if (!__x._M_engaged) return true;") != std::string::npos);
    CHECK(eq_body.find("return __x._M_storage._M_value == __y._M_storage._M_value;") != std::string::npos);
    CHECK(eq_body.find("if (!__x._M_engaged) return true;") < eq_body.find("_M_value"));
    CHECK(lt_body.find("if (!__y._M_engaged) return false;") != std::string::npos);
    CHECK(lt_body.find("if (!__x._M_engaged) return true;") != std::string::npos);
    CHECK(lt_body.find("return __x._M_storage._M_value < __y._M_storage._M_value;") != std::string::npos);
    const auto lt_value = lt_body.find("_M_value");
    REQUIRE(lt_value != std::string::npos);
    CHECK(lt_body.find("if (!__y._M_engaged) return false;") < lt_value);
    CHECK(lt_body.find("if (!__x._M_engaged) return true;") < lt_value);
    for (const std::string& body : {eq_body, lt_body}) {
        CHECK(body.find("value()") == std::string::npos);
        CHECK(body.find("operator*") == std::string::npos);
        CHECK(body.find("_Tp{}") == std::string::npos);
        CHECK(body.find("_Tp()") == std::string::npos);
        CHECK(body.find("T{}") == std::string::npos);
        CHECK(body.find("__prism_check") == std::string::npos);
    }

    const std::string code = strip_comments(src);
    CHECK(code.find("emplace") == std::string::npos);
    CHECK(code.find("hash") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("bool operator==(") != std::string::npos);
    CHECK(code.find("bool operator<(") != std::string::npos);
    CHECK(code.find("void reset() noexcept") != std::string::npos);
    CHECK(code.find("value_or") != std::string::npos);
    CHECK(code.find("and_then") != std::string::npos);
    CHECK(code.find("or_else") != std::string::npos);
    CHECK(code.find("transform") != std::string::npos);
    CHECK(code.find("CXX-OPTIONAL-NULL") != std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <functional>") == std::string::npos);
    CHECK(code.find("struct _Prism_optional_kind<optional<_T>>") != std::string::npos);
}

TEST_CASE("cxx optional model: two empty optionals are equal and an empty one is less") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = c.tmp / "optional_compare.cpp";
    // Box has no default constructor. Comparing two empty optionals must not
    // build Box{}. operator== and operator< print, so a read of an inactive
    // Box shows up as eq or lt before any engaged pair.
    std::ofstream(src) << "#include <cstdio>\n"
                          "#include <optional>\n"
                          "struct Box {\n"
                          "    int v;\n"
                          "    Box() = delete;\n"
                          "    explicit Box(int x) : v(x) {}\n"
                          "    bool operator==(const Box& o) const {\n"
                          "        std::printf(\"eq %d %d\\n\", v, o.v);\n"
                          "        return v == o.v;\n"
                          "    }\n"
                          "    bool operator<(const Box& o) const {\n"
                          "        std::printf(\"lt %d %d\\n\", v, o.v);\n"
                          "        return v < o.v;\n"
                          "    }\n"
                          "};\n"
                          "int main() {\n"
                          "    std::optional<Box> e1;\n"
                          "    std::optional<Box> e2;\n"
                          "    std::printf(\"ee eq=%d\\n\", static_cast<int>(e1 == e2));\n"
                          "    std::printf(\"ee lt=%d\\n\", static_cast<int>(e1 < e2));\n"
                          "    std::printf(\"ee rev=%d\\n\", static_cast<int>(e2 < e1));\n"
                          "    std::optional<Box> one(Box(1));\n"
                          "    std::printf(\"de eq=%d\\n\", static_cast<int>(e1 == one));\n"
                          "    std::printf(\"de lt=%d\\n\", static_cast<int>(e1 < one));\n"
                          "    std::printf(\"de rev=%d\\n\", static_cast<int>(one < e1));\n"
                          "    std::optional<Box> one_b(Box(1));\n"
                          "    std::printf(\"same eq=%d\\n\", static_cast<int>(one == one_b));\n"
                          "    std::printf(\"same lt=%d\\n\", static_cast<int>(one < one_b));\n"
                          "    std::printf(\"same rev=%d\\n\", static_cast<int>(one_b < one));\n"
                          "    std::optional<Box> two(Box(2));\n"
                          "    std::printf(\"ord eq=%d\\n\", static_cast<int>(one == two));\n"
                          "    std::printf(\"ord lt=%d\\n\", static_cast<int>(one < two));\n"
                          "    std::printf(\"ord rev=%d\\n\", static_cast<int>(two < one));\n"
                          "    const std::optional<Box> cempty;\n"
                          "    const std::optional<Box> ctwo(Box(2));\n"
                          "    const std::optional<Box> ctwo_b(Box(2));\n"
                          "    std::printf(\"cde eq=%d\\n\", static_cast<int>(cempty == ctwo));\n"
                          "    std::printf(\"cde lt=%d\\n\", static_cast<int>(cempty < ctwo));\n"
                          "    std::printf(\"cde rev=%d\\n\", static_cast<int>(ctwo < cempty));\n"
                          "    std::printf(\"c22 eq=%d\\n\", static_cast<int>(ctwo == ctwo_b));\n"
                          "    std::printf(\"c22 lt=%d\\n\", static_cast<int>(ctwo < ctwo_b));\n"
                          "    std::printf(\"c22 rev=%d\\n\", static_cast<int>(ctwo_b < ctwo));\n"
                          "    std::optional<int> ie;\n"
                          "    std::optional<int> iz(0);\n"
                          "    std::optional<int> iz2(0);\n"
                          "    std::optional<int> i1(1);\n"
                          "    std::optional<int> ineg(-1);\n"
                          "    std::printf(\"iz eq=%d\\n\", static_cast<int>(ie == iz));\n"
                          "    std::printf(\"iz lt=%d\\n\", static_cast<int>(ie < iz));\n"
                          "    std::printf(\"iz rev=%d\\n\", static_cast<int>(iz < ie));\n"
                          "    std::printf(\"zz eq=%d\\n\", static_cast<int>(iz == iz2));\n"
                          "    std::printf(\"zz lt=%d\\n\", static_cast<int>(iz < iz2));\n"
                          "    std::printf(\"zz rev=%d\\n\", static_cast<int>(iz2 < iz));\n"
                          "    std::printf(\"01 eq=%d\\n\", static_cast<int>(iz == i1));\n"
                          "    std::printf(\"01 lt=%d\\n\", static_cast<int>(iz < i1));\n"
                          "    std::printf(\"01 rev=%d\\n\", static_cast<int>(i1 < iz));\n"
                          "    std::printf(\"neg eq=%d\\n\", static_cast<int>(ineg == iz));\n"
                          "    std::printf(\"neg lt=%d\\n\", static_cast<int>(ineg < iz));\n"
                          "    std::printf(\"neg rev=%d\\n\", static_cast<int>(iz < ineg));\n"
                          "    std::printf(\"ii eq=%d\\n\", static_cast<int>(ie == ie));\n"
                          "    std::printf(\"ii lt=%d\\n\", static_cast<int>(ie < ie));\n"
                          "    std::printf(\"ee ne=%d\\n\", static_cast<int>(e1 != e2));\n"
                          "    std::printf(\"de ne=%d\\n\", static_cast<int>(e1 != one));\n"
                          "    std::printf(\"same ne=%d\\n\", static_cast<int>(one != one_b));\n"
                          "    std::printf(\"ord ne=%d\\n\", static_cast<int>(one != two));\n"
                          "    std::printf(\"ii ne=%d\\n\", static_cast<int>(ie != ie));\n"
                          "    std::printf(\"iz ne=%d\\n\", static_cast<int>(ie != iz));\n"
                          "    std::printf(\"zz ne=%d\\n\", static_cast<int>(iz != iz2));\n"
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

    auto lib = run_lines(build_value_or(c.cxx, c.tmp, src, false, "compare_trace"));
    auto mod = run_lines(build_value_or(c.cxx, c.tmp, src, true, "compare_trace"));
    const std::vector<std::string> want{
        "ee eq=1", "ee lt=0", "ee rev=0", "de eq=0", "de lt=1", "de rev=0", "eq 1 1", "same eq=1", "lt 1 1",
        "same lt=0", "lt 1 1", "same rev=0", "eq 1 2", "ord eq=0", "lt 1 2", "ord lt=1", "lt 2 1", "ord rev=0",
        "cde eq=0", "cde lt=1", "cde rev=0", "eq 2 2", "c22 eq=1", "lt 2 2", "c22 lt=0", "lt 2 2", "c22 rev=0",
        "iz eq=0", "iz lt=1", "iz rev=0", "zz eq=1", "zz lt=0", "zz rev=0", "01 eq=0", "01 lt=1", "01 rev=0",
        "neg eq=0", "neg lt=1", "neg rev=0", "ii eq=1", "ii lt=0", "ee ne=0", "de ne=1", "eq 1 1", "same ne=0",
        "eq 1 2", "ord ne=1", "ii ne=0", "iz ne=1", "zz ne=0",
    };
    CHECK(lib == mod);
    CHECK(mod == want);
    for (const auto& line : mod) {
        CHECK(line != "ee eq=0");
        CHECK(line != "ee lt=1");
        CHECK(line != "ee rev=1");
        CHECK(line != "de eq=1");
        CHECK(line != "de lt=0");
        CHECK(line != "de rev=1");
        CHECK(line != "same eq=0");
        CHECK(line != "same lt=1");
        CHECK(line != "same rev=1");
        CHECK(line != "ord eq=1");
        CHECK(line != "ord lt=0");
        CHECK(line != "ord rev=1");
        CHECK(line != "cde eq=1");
        CHECK(line != "cde lt=0");
        CHECK(line != "cde rev=1");
        CHECK(line != "c22 eq=0");
        CHECK(line != "c22 lt=1");
        CHECK(line != "iz eq=1");
        CHECK(line != "iz lt=0");
        CHECK(line != "iz rev=1");
        CHECK(line != "zz eq=0");
        CHECK(line != "zz lt=1");
        CHECK(line != "01 eq=1");
        CHECK(line != "01 lt=0");
        CHECK(line != "01 rev=1");
        CHECK(line != "neg eq=1");
        CHECK(line != "neg lt=0");
        CHECK(line != "neg rev=1");
        CHECK(line != "ii eq=0");
        CHECK(line != "ii lt=1");
        CHECK(line != "ee ne=1");
        CHECK(line != "de ne=0");
        CHECK(line != "same ne=1");
        CHECK(line != "ord ne=0");
        CHECK(line != "ii ne=1");
        CHECK(line != "iz ne=0");
        CHECK(line != "zz ne=1");
        CHECK(line != "eq 0 0");
        CHECK(line != "lt 0 0");
        CHECK(line != "eq 0 1");
        CHECK(line != "lt 0 1");
        CHECK(line != "eq 1 0");
        CHECK(line != "lt 0 2");
    }
}

TEST_CASE("cxx optional model: comparison of two different types is not declared") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path src = c.tmp / "optional_compare_hetero.cpp";
    std::ofstream(src) << "#include <optional>\n"
                          "int main() {\n"
                          "    std::optional<int> a(1);\n"
                          "    std::optional<long> b(1L);\n"
                          "    return (a == b || a < b) ? 0 : 1;\n"
                          "}\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-w"};
    auto compile = [&](bool with_models) {
        std::vector<std::string> argv{c.cxx};
        argv.insert(argv.end(), flags.begin(), flags.end());
        if (with_models) {
            argv.push_back("-isystem");
            argv.push_back(models.string());
        }
        argv.push_back(src.string());
        argv.push_back("-o");
        argv.push_back((c.tmp / (with_models ? "compare_hetero.model" : "compare_hetero.lib")).string());
        return run_cmd(argv);
    };
    CHECK(compile(false));
    CHECK_FALSE(compile(true));
}
