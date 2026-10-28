// The std::syncstream model header (src/prism/pir/models/cxx/syncstream) is a
// file on the include path. CMake's prism_tests source list is explicit (not
// a glob): this file is one line in _prism_test_sources. The trace is
// tests/cxx_models/syncstream_trace.cpp.
//
// A write at the size is a __prism_check. A read at the size is a
// __prism_check and does not load the pointer. sungetc at offset 0 is a
// __prism_check and does not decrement or load before the buffer. The
// header does not define emit.

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

fs::path syncstream_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "syncstream"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// `name` as its own identifier, then '('. A comment that only says emit does not count.
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

struct SyncCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

SyncCtx& ctx() {
    static SyncCtx c;
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
    std::ofstream(probe) << "#include <syncstream>\n"
                            "int main() { return 0; }\n";
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

fs::path build_prog(const std::string& cxx, const fs::path& tmp) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    fs::path out = tmp / "syncstream_trace.model";
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back((repo() / "tests" / "cxx_models" / "syncstream_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

std::vector<std::string> run_lines(const fs::path& exe, const char* arg = nullptr) {
    std::string stdout_text;
    std::vector<std::string> cmd{exe.string()};
    if (arg != nullptr) cmd.push_back(arg);
    REQUIRE(run_cmd(cmd, &stdout_text));
    std::vector<std::string> lines;
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

bool is_check(const fs::path& exe, const char* arg, const char* cls, const char* msg, const char* banned) {
    std::string out, err;
    if (run_cmd({exe.string(), arg}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    if (out.find(banned) != std::string::npos) return false;
    return true;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

// One in-range sgetc prints `need`, then the next call is a check and does not print `banned`.
bool read_then_check(const fs::path& exe, const char* arg, const char* need, const char* banned, const char* cls,
                     const char* msg) {
    std::string out, err;
    if (run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(need) == std::string::npos) return false;
    if (out.find(banned) != std::string::npos) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    return true;
}

// One failing check is reported, the process still exits 0, `line` is printed,
// and `absent` is not in stdout or stderr.
bool one_check_then_line(const fs::path& exe, const char* arg, const char* line, const char* cls, const char* msg,
                         const char* absent) {
    std::string out, err;
    if (!run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(line) == std::string::npos) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    if (both.find(absent) != std::string::npos) return false;
    return true;
}

// The constructor check runs, then sgetc's check, and the process still prints `line`.
bool checks_then_line(const fs::path& exe, const char* arg, const char* line, const char* first, const char* second) {
    std::string out, err;
    if (!run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(line) == std::string::npos) return false;
    const auto a = err.find(first);
    const auto b = err.find(second);
    if (a == std::string::npos || b == std::string::npos) return false;
    return a < b;
}

}  // namespace

TEST_CASE("cxx syncstream model: a write or a read at the size is a check, a step back at offset 0 is a check, and "
          "emit is not defined") {
    const fs::path header = syncstream_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("_GLIBCXX_SYNCSTREAM") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("fails to compile and pir falls back") != std::string::npos);
    CHECK(src.find("_PRISM_SYNCSTREAM_MODEL") != std::string::npos);
    CHECK(src.find("#include <cstddef>") != std::string::npos);
    CHECK(src.find("#include <span>") == std::string::npos);
    CHECK(src.find("#include <sstream>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <ostream>") == std::string::npos);
    CHECK(src.find("#include <spanstream>") == std::string::npos);
    CHECK(src.find("#include <mutex>") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("free(") == std::string::npos);
    CHECK(src.find("printf") == std::string::npos);
    CHECK(src.find("std::cout") == std::string::npos);
    CHECK(src.find("char* _M_buf;") != std::string::npos);
    CHECK(src.find("size_t _M_size;") != std::string::npos);
    CHECK(src.find("size_t _M_off;") != std::string::npos);
    CHECK(src.find("~basic_syncbuf() = default;") != std::string::npos);

    const auto ctor = src.find("explicit basic_syncbuf(");
    REQUIRE(ctor != std::string::npos);
    const auto sputc_at = src.find("void sputc(", ctor);
    REQUIRE(sputc_at != std::string::npos);
    const auto sgetc_at = src.find("int sgetc(", sputc_at);
    REQUIRE(sgetc_at != std::string::npos);
    const std::string ctor_body = src.substr(ctor, sputc_at - ctor);
    CHECK(ctor_body.find("__prism_check((__buf != nullptr || __n == 0) ? 1 : 0, \"FUNC-CONTRACT\",") !=
          std::string::npos);
    CHECK(ctor_body.find("basic_syncbuf constructed with a null buffer and a non-zero size") != std::string::npos);
    CHECK(ctor_body.find("if (!(__buf != nullptr || __n == 0)) _M_size = 0;") != std::string::npos);

    const std::string sputc = src.substr(sputc_at, sgetc_at - sputc_at);
    CHECK(sputc.find("const int __ok = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(sputc.find("__prism_check(__ok, \"MEM-OOB-WRITE\", \"basic_syncbuf::sputc would write past the caller "
                     "buffer\")") != std::string::npos);
    const auto check_at = sputc.find("__prism_check");
    const auto write_at = sputc.find("_M_buf[_M_off] = __c;");
    const auto adv_at = sputc.find("++_M_off;");
    REQUIRE(check_at != std::string::npos);
    REQUIRE(write_at != std::string::npos);
    REQUIRE(adv_at != std::string::npos);
    CHECK(check_at < write_at);
    CHECK(write_at < adv_at);
    CHECK(sputc.find("if (__ok) {") != std::string::npos);
    CHECK(sputc.find("_M_off =") == std::string::npos);
    CHECK(sputc.find("++_M_off;", adv_at + 1) == std::string::npos);

    const auto sungetc_at = src.find("int sungetc(", sgetc_at);
    REQUIRE(sungetc_at != std::string::npos);
    const std::string sgetc = src.substr(sgetc_at, sungetc_at - sgetc_at);
    CHECK(sgetc.find("const int __in = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(sgetc.find("__prism_check(__in, \"MEM-OOB-READ\", \"basic_syncbuf::sgetc would read past the caller "
                     "buffer\")") != std::string::npos);
    const auto gcheck = sgetc.find("__prism_check");
    const auto gread = sgetc.find("static_cast<unsigned char>(_M_buf[_M_off])");
    const auto gadv = sgetc.find("++_M_off;");
    const auto gret = sgetc.find("return static_cast<int>(__ch);");
    const auto geof = sgetc.find("return -1;");
    REQUIRE(gcheck != std::string::npos);
    REQUIRE(gread != std::string::npos);
    REQUIRE(gadv != std::string::npos);
    REQUIRE(gret != std::string::npos);
    REQUIRE(geof != std::string::npos);
    CHECK(gcheck < gread);
    CHECK(gread < gadv);
    CHECK(gadv < gret);
    CHECK(gret < geof);
    CHECK(sgetc.find("if (__in) {") != std::string::npos);
    CHECK(sgetc.find("_M_buf[", sgetc.find("_M_buf[") + 1) == std::string::npos);
    CHECK(sgetc.find("return -1;", geof + 1) == std::string::npos);
    CHECK(sgetc.find("_M_off =") == std::string::npos);
    CHECK(sgetc.find("++_M_off;", gadv + 1) == std::string::npos);

    const auto priv = src.find("private:", sungetc_at);
    REQUIRE(priv != std::string::npos);
    const std::string sungetc = src.substr(sungetc_at, priv - sungetc_at);
    CHECK(sungetc.find("const int __back = (_M_off > 0) ? 1 : 0;") != std::string::npos);
    CHECK(sungetc.find("__prism_check(__back, \"MEM-OOB-READ\", \"basic_syncbuf::sungetc would read before the caller "
                       "buffer\")") != std::string::npos);
    const auto ucheck = sungetc.find("__prism_check");
    const auto udec = sungetc.find("--_M_off;");
    const auto uread = sungetc.find("static_cast<unsigned char>(_M_buf[_M_off])");
    const auto uret = sungetc.find("return static_cast<int>(__ch);");
    const auto ueof = sungetc.find("return -1;");
    REQUIRE(ucheck != std::string::npos);
    REQUIRE(udec != std::string::npos);
    REQUIRE(uread != std::string::npos);
    REQUIRE(uret != std::string::npos);
    REQUIRE(ueof != std::string::npos);
    CHECK(ucheck < udec);
    CHECK(udec < uread);
    CHECK(uread < uret);
    CHECK(uret < ueof);
    CHECK(sungetc.find("if (__back) {") != std::string::npos);
    CHECK(sungetc.find("_M_buf[", sungetc.find("_M_buf[") + 1) == std::string::npos);
    CHECK(sungetc.find("return -1;", ueof + 1) == std::string::npos);
    CHECK(sungetc.find("_M_off =") == std::string::npos);
    CHECK(sungetc.find("--_M_off;", udec + 1) == std::string::npos);
    CHECK(sungetc.find("++_M_off") == std::string::npos);

    CHECK(defines_call(src, "sputc"));
    CHECK(defines_call(src, "sgetc"));
    CHECK(defines_call(src, "sungetc"));
    CHECK_FALSE(defines_call(src, "emit"));
    CHECK(src.find("emit(") == std::string::npos);
    CHECK(src.find("class syncstream") == std::string::npos);
    CHECK(src.find("osyncstream") == std::string::npos);
}

TEST_CASE("cxx syncstream model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxsyncstream-doctest";
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

TEST_CASE("cxx syncstream model: one stored byte stays in the caller buffer") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "syncstream_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(syncstream_header().string())) != std::string::npos);

    auto mod = build_prog(c.cxx, c.tmp);
    auto lines = run_lines(mod);
    CHECK(has_line(lines, "byte 65"));
    CHECK(has_line(lines, "kept 65"));
    CHECK(is_check(mod, "full", "MEM-OOB-WRITE", "basic_syncbuf::sputc would write past the caller buffer", "wrote"));
    CHECK(is_check(mod, "wrap", "MEM-OOB-WRITE", "basic_syncbuf::sputc would write past the caller buffer", "wrapped"));
    CHECK(is_check(mod, "null", "FUNC-CONTRACT", "basic_syncbuf constructed with a null buffer and a non-zero size",
                   "constructed"));
    auto got = run_lines(mod, "get");
    CHECK(has_line(got, "got 0"));
    CHECK(has_line(got, "got 255"));
    CHECK(has_line(got, "got 65"));
    CHECK(is_check(mod, "empty", "MEM-OOB-READ", "basic_syncbuf::sgetc would read past the caller buffer", "got"));
    CHECK(is_check(mod, "null0", "MEM-OOB-READ", "basic_syncbuf::sgetc would read past the caller buffer", "read"));
    CHECK(read_then_check(mod, "past", "once 65", "past", "MEM-OOB-READ",
                          "basic_syncbuf::sgetc would read past the caller buffer"));
    CHECK(checks_then_line(mod, "forced", "forced -1",
                           "CHECK FUNC-CONTRACT: basic_syncbuf constructed with a null buffer and a non-zero size",
                           "CHECK MEM-OOB-READ: basic_syncbuf::sgetc would read past the caller buffer"));
    auto back = run_lines(mod, "back");
    CHECK(has_line(back, "back 65"));
    CHECK(has_line(back, "back 255"));
    CHECK(has_line(back, "back 0"));
    CHECK(is_check(mod, "before", "MEM-OOB-READ", "basic_syncbuf::sungetc would read before the caller buffer",
                   "before"));
    CHECK(is_check(mod, "unnull0", "MEM-OOB-READ", "basic_syncbuf::sungetc would read before the caller buffer",
                   "unread"));
    CHECK(read_then_check(mod, "step", "step 65", "under", "MEM-OOB-READ",
                          "basic_syncbuf::sungetc would read before the caller buffer"));
    CHECK(checks_then_line(mod, "unforced", "unforced -1",
                           "CHECK FUNC-CONTRACT: basic_syncbuf constructed with a null buffer and a non-zero size",
                           "CHECK MEM-OOB-READ: basic_syncbuf::sungetc would read before the caller buffer"));
    CHECK(one_check_then_line(mod, "stay", "stayed -1 65", "MEM-OOB-READ",
                              "basic_syncbuf::sungetc would read before the caller buffer", "MEM-OOB-WRITE"));
}
