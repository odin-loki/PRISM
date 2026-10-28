// The std::latch model header (src/prism/pir/models/cxx/latch) is a file on
// the include path. CMake's prism_tests source list is explicit (not a
// glob): this file is one line in _prism_test_sources. The trace is
// tests/cxx_models/latch_trace.cpp.
//
// count_down past zero is a __prism_check. try_wait is false while the
// count is positive. max() returns PTRDIFF_MAX and does not change the
// count. The header does not define wait(.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
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

fs::path latch_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "latch"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// `name` as its own identifier, then '('. try_wait does not define wait.
bool defines_call(std::string_view src, std::string_view name) {
    for (std::size_t i = 0; i + name.size() <= src.size(); ++i) {
        if (src.compare(i, name.size(), name) != 0) continue;
        if (i > 0 && is_ident(src[i - 1])) continue;
        std::size_t j = i + name.size();
        if (j < src.size() && is_ident(src[j])) continue;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) ++j;
        if (j < src.size() && src[j] == '(') return true;
    }
    return false;
}

struct LatchCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

LatchCtx& ctx() {
    static LatchCtx c;
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
    std::ofstream(probe) << "#include <latch>\n"
                            "int main() {\n"
                            "  std::latch g(1);\n"
                            "  g.count_down();\n"
                            "  return g.try_wait() ? 0 : 1;\n"
                            "}\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
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
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    fs::path out = tmp / (with_models ? "latch_trace.model" : "latch_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "latch_trace.cpp").string());
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

bool over_is_check(const fs::path& exe) {
    std::string out, err;
    if (run_cmd({exe.string(), "over"}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK FUNC-CONTRACT:") == std::string::npos) return false;
    if (both.find("latch::count_down would pass below zero") == std::string::npos) return false;
    if (out.find("wrapped") != std::string::npos) return false;
    return true;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

}  // namespace

TEST_CASE("cxx latch model: count_down past zero is a check and wait is not defined") {
    const fs::path header = latch_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("_GLIBCXX_LATCH") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("ptrdiff_t _M_count;") != std::string::npos);

    const auto cd = src.find("void count_down(");
    REQUIRE(cd != std::string::npos);
    const auto tw = src.find("bool try_wait(", cd);
    REQUIRE(tw != std::string::npos);
    const std::string count_down = src.substr(cd, tw - cd);
    CHECK(count_down.find("__prism_check(__ok, \"FUNC-CONTRACT\", \"latch::count_down would pass below zero\")") !=
          std::string::npos);
    const auto check_at = count_down.find("__prism_check");
    const auto sub_at = count_down.find("_M_count -=");
    REQUIRE(check_at != std::string::npos);
    REQUIRE(sub_at != std::string::npos);
    CHECK(check_at < sub_at);
    CHECK(count_down.find("if (__ok) _M_count -= __n;") != std::string::npos);
    CHECK(count_down.find("_M_count -=", sub_at + 1) == std::string::npos);

    const auto tw_end = src.find('}', tw);
    REQUIRE(tw_end != std::string::npos);
    const std::string try_wait = src.substr(tw, tw_end - tw);
    CHECK(try_wait.find("return _M_count == 0;") != std::string::npos);
    CHECK(try_wait.find("-=") == std::string::npos);
    CHECK(try_wait.find("count_down") == std::string::npos);
    CHECK(try_wait.find("return true") == std::string::npos);

    CHECK(defines_call(src, "try_wait"));
    CHECK(defines_call(src, "count_down"));
    CHECK(defines_call(src, "max"));
    CHECK_FALSE(defines_call(src, "wait"));
    CHECK_FALSE(defines_call(src, "arrive_and_wait"));

    const auto mx = src.find("static constexpr ptrdiff_t max() noexcept");
    REQUIRE(mx != std::string::npos);
    CHECK(mx < cd);
    const auto mx_end = src.find('}', mx);
    REQUIRE(mx_end != std::string::npos);
    const std::string max_fn = src.substr(mx, mx_end - mx);
    CHECK(max_fn.find("return PTRDIFF_MAX;") != std::string::npos);
    CHECK(max_fn.find("_M_count") == std::string::npos);
    CHECK(max_fn.find("-=") == std::string::npos);
    CHECK(max_fn.find("count_down") == std::string::npos);
    CHECK(max_fn.find("wait") == std::string::npos);
    CHECK(src.find("max() is modelled") != std::string::npos);
    CHECK(src.find("wait and arrive_and_wait stay omitted") != std::string::npos);
    CHECK(src.find("max, wait, and arrive_and_wait are omitted") == std::string::npos);
    CHECK(src.find("#include <cstdint>") != std::string::npos);
}

TEST_CASE("cxx latch model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxlatch-doctest";
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

TEST_CASE("cxx latch model: try_wait is false while the count is positive") {
    auto& c = ctx();
    if (!c.ok) return;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "latch_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(latch_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "start 0"));
    CHECK(has_line(b, "mid 0"));
    CHECK(has_line(b, "zero 1"));
    CHECK(has_line(b, "empty 1"));
    CHECK(over_is_check(mod));
}
