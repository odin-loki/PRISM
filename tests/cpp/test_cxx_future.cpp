// The std::future model header (src/prism/pir/models/cxx/future) is a file on
// the include path, same shape as tests/cpp/test_cxx_optional.cpp. CMake's
// prism_tests source list is explicit (not a glob): this file is one line in
// _prism_test_sources. docs/finish-notes/future.md has the line that links it.
// The trace is tests/cxx_models/future_trace.cpp.
//
// get() with no stored value is a __prism_check and does not return a
// value-initialised T. async and wait( are not defined.

#include <doctest/doctest.h>

#include "prose_text.hpp"

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

fs::path future_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "future"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Comments and string literals are not definitions.
std::string strip_comments(const std::string& s) {
    std::string o;
    o.reserve(s.size());
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
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else ++i;
            }
            if (i < s.size()) ++i;
            continue;
        }
        o.push_back(s[i]);
        ++i;
    }
    return o;
}

std::string function_body(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    REQUIRE(at != std::string::npos);
    const auto brace = src.find('{', at);
    REQUIRE(brace != std::string::npos);
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    FAIL("unterminated function body");
    return {};
}

struct FutCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

FutCtx& ctx() {
    static FutCtx c;
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
    std::ofstream(probe) << "#include <future>\n"
                            "int main() {\n"
                            "  std::promise<int> p;\n"
                            "  std::future<int> f = p.get_future();\n"
                            "  p.set_value(1);\n"
                            "  return f.get() - 1;\n"
                            "}\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
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
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
    fs::path out = tmp / (with_models ? "future_trace.model" : "future_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "future_trace.cpp").string());
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

bool check_aborts(const fs::path& exe, const char* mode, const char* cls) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx future model: empty get() is a check and the header does not define async or wait(") {
    const fs::path header = future_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_FUTURE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_FUTURE") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("pir falls back") != std::string::npos);
    CHECK(src.find("fake synchronization") != std::string::npos);
    for (const char* inc : {"#include <memory>", "#include <thread>", "#include <functional>", "#include <mutex>"})
        CHECK(src.find(inc) == std::string::npos);

    const std::string get = function_body(src, "_Tp get()");
    const auto check = get.find("__prism_check(");
    const auto ret = get.find("return __out");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(get.find("CXX-FUTURE-GET") != std::string::npos);
    CHECK(get.find("_M_state") != std::string::npos);
    CHECK(get.find("_M_retrieved") != std::string::npos);
    CHECK(get.find("return {}") == std::string::npos);
    CHECK(get.find("return _Tp") == std::string::npos);
    CHECK(get.find("_Tp{}") == std::string::npos);
    CHECK(get.find("_Tp()") == std::string::npos);
    CHECK(get.find("value_type{}") == std::string::npos);
    CHECK(get.find("value_type()") == std::string::npos);

    for (const char* sig : {"void set_value(const _Tp&", "void set_value(_Tp&&"}) {
        const std::string set = function_body(src, sig);
        const auto set_check = set.find("__prism_check(");
        const auto set_new = set.find("new _Tp");
        REQUIRE(set_check != std::string::npos);
        REQUIRE(set_new != std::string::npos);
        CHECK(set_check < set_new);
        CHECK(set.find("if (!__ok) return;") != std::string::npos);
        CHECK(set.find("FUNC-CONTRACT") != std::string::npos);
    }

    const std::string valid = function_body(src, "bool valid()");
    CHECK(valid.find("_M_state != nullptr") != std::string::npos);

    const std::string code = strip_comments(src);
    CHECK(code.find("async") == std::string::npos);
    CHECK(code.find("wait(") == std::string::npos);
    CHECK(code.find("wait_for") == std::string::npos);
    CHECK(code.find("wait_until") == std::string::npos);
    CHECK(code.find("packaged_task") == std::string::npos);
}

TEST_CASE("cxx future model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxfuture-doctest";
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

TEST_CASE("cxx future model: one stored value matches libstdc++ and empty get() is a check") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w", "-pthread"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "future_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(future_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(b == std::vector<std::string>{"empty 0", "live 1", "got 42 0", "held 5"});

    CHECK(check_aborts(mod, "empty", "CXX-FUTURE-GET"));
    CHECK(check_aborts(mod, "twice", "FUNC-CONTRACT"));
    CHECK(check_aborts(mod, "get2", "CXX-FUTURE-GET"));
}
