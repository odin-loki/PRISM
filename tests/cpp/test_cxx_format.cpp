// Differential test of the std::format model header
// (src/prism/pir/models/cxx/format). Same shape as tests/cpp/test_cxx_list.cpp:
// the trace is compiled twice, once against libstdc++ and once with -isystem
// on the model directory so `#include <format>` is this header. CMake's
// prism_tests source list is explicit (not a glob).
//
// A too-small n does not make format_to_n report a zero size for a non-zero
// int. A null output pointer with n > 0 is __prism_check in the header.
// long long uses the same writer, in decimal, only for "{}". A length that
// does not fit in size_t is __prism_check then __builtin_trap.

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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "format"; }

struct FmtCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

FmtCtx& ctx() {
    static FmtCtx c;
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

#ifdef _WIN32
constexpr const char* kProbeExe = "probe.exe";
#else
constexpr const char* kProbeExe = "probe";
#endif

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <format>\n"
                            "int main() {\n"
                            "  char buf[8] = {};\n"
                            "  auto r = std::format_to_n(buf, 8, \"{}\", 42);\n"
                            "  return (r.size == 2 && buf[0] == '4' && buf[1] == '2') ? 0 : 1;\n"
                            "}\n";
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
    fs::path out = tmp / (with_models ? "format_trace.model" : "format_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "format_trace.cpp").string());
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

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("cxx format model: header checks a null output pointer") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_FORMAT") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("#include <string>") == std::string::npos);
    CHECK(text.find("#include <string_view>") == std::string::npos);
    CHECK(text.find("#include <sstream>") == std::string::npos);
    CHECK(text.find("#include <vector>") == std::string::npos);
    CHECK(text.find("fail to compile") != std::string::npos);
    CHECK(text.find("falls back") != std::string::npos);
    const auto fn = text.find("format_to_n(");
    REQUIRE(fn != std::string::npos);
    const auto check = text.find("__prism_check(", fn);
    REQUIRE(check != std::string::npos);
    const auto call_end = text.find(");", check);
    REQUIRE(call_end != std::string::npos);
    const std::string call = text.substr(check, call_end - check);
    CHECK(call.find("nullptr") != std::string::npos);
    CHECK(call.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(call.find("null") != std::string::npos);
    const auto guard = text.rfind("if", check);
    REQUIRE(guard != std::string::npos);
    CHECK(text.substr(guard, check - guard).find("__n > 0") != std::string::npos);
}

TEST_CASE("cxx format model: long long shares the writer and traps a length that does not fit") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <string>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(text.find("\nformat(") == std::string::npos);
    CHECK(text.find("\nformat_to(") == std::string::npos);
    CHECK(text.find("std::format(") == std::string::npos);

    const auto write_at = text.find("inline char* __prism_format_write(");
    const auto int_at = text.find("format_to_n(char* __outv, std::size_t __n, const char* __fmt, int __v)");
    const auto ll_at =
        text.find("format_to_n(char* __outv, std::size_t __n, const char* __fmt, long long __v)");
    REQUIRE(write_at != std::string::npos);
    REQUIRE(int_at != std::string::npos);
    REQUIRE(ll_at != std::string::npos);
    CHECK(write_at < int_at);
    CHECK(int_at < ll_at);

    const std::string writer = text.substr(write_at, int_at - write_at);
    const std::string ibody = text.substr(int_at, ll_at - int_at);
    const std::string lbody = text.substr(ll_at);
    CHECK(writer.find("The unwritten tail is not stored.") != std::string::npos);
    CHECK(writer.find("for (std::size_t __k = 0; __k < __put; ++__k) __outv[__k] = __tmp[__k];") !=
          std::string::npos);
    CHECK(writer.find("__k < __len") == std::string::npos);
    CHECK(writer.find("__k < __n") == std::string::npos);
    CHECK(ibody.find("unsigned __mag = __neg ? static_cast<unsigned>(0) - static_cast<unsigned>(__v) : "
                     "static_cast<unsigned>(__v);") != std::string::npos);
    CHECK(ibody.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(ibody.find("__prism_format_write(") != std::string::npos);
    CHECK(ibody.find("__outv[__k]") == std::string::npos);
    CHECK(ibody.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);
    CHECK(ibody.find("format_to_n length does not fit in size_t") == std::string::npos);

    const auto brace = lbody.find('{');
    const auto zero = lbody.find("if (__n == 0)");
    REQUIRE(brace != std::string::npos);
    REQUIRE(zero != std::string::npos);
    const std::string before_zero = lbody.substr(brace, zero - brace);
    CHECK(before_zero.find("__outv") == std::string::npos);
    CHECK(before_zero.find("__prism_format_write") == std::string::npos);
    const auto zero_ret = lbody.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = lbody.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __outv;") != std::string::npos);
    CHECK(zero_arm.find("__outv[") == std::string::npos);
    CHECK(zero_arm.find("__prism_format_write") == std::string::npos);
    CHECK(zero_arm.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);

    const auto len_check =
        lbody.find("__prism_check(0, \"FUNC-CONTRACT\", \"format_to_n length does not fit in size_t\")");
    REQUIRE(len_check != std::string::npos);
    const auto len_trap = lbody.find("__builtin_trap()", len_check);
    const auto len_else = lbody.find("else", len_trap);
    const auto len_add = lbody.find("__len = __nd + __sign;", len_else);
    REQUIRE(len_trap != std::string::npos);
    REQUIRE(len_else != std::string::npos);
    REQUIRE(len_add != std::string::npos);
    CHECK(len_check < len_trap);
    CHECK(len_trap < len_else);
    CHECK(len_else < len_add);
    CHECK(len_add < zero);
    const std::string len_arm = lbody.substr(len_check, len_trap - len_check);
    CHECK(len_arm.find("__prism_format_write") == std::string::npos);
    CHECK(len_arm.find("__outv") == std::string::npos);
    CHECK(len_arm.find("__nd + __sign") == std::string::npos);
    CHECK(len_arm.find("return") == std::string::npos);

    const auto fmt_trap = lbody.find("if (!__fmt_ok) __builtin_trap();");
    const auto null_msg = lbody.find("format_to_n called with a null output pointer");
    REQUIRE(fmt_trap != std::string::npos);
    REQUIRE(null_msg != std::string::npos);
    const auto null_call = lbody.rfind("__prism_check(", null_msg);
    const auto null_end = lbody.find(");", null_msg);
    REQUIRE(null_call != std::string::npos);
    REQUIRE(null_end != std::string::npos);
    const std::string null_txt = lbody.substr(null_call, null_end - null_call);
    CHECK(null_txt.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(null_txt.find("nullptr") != std::string::npos);
    CHECK(null_txt.find("null") != std::string::npos);
    const auto null_trap = lbody.find("if (__outv == nullptr) __builtin_trap();", null_end);
    const auto call = lbody.find("__prism_format_write(");
    REQUIRE(null_trap != std::string::npos);
    REQUIRE(call != std::string::npos);
    CHECK(fmt_trap < zero);
    CHECK(zero < null_call);
    CHECK(null_call < null_trap);
    CHECK(null_trap < call);
    const std::string null_arm = lbody.substr(null_call, null_trap - null_call);
    CHECK(null_arm.find("__prism_format_write") == std::string::npos);
    CHECK(null_arm.find("__outv[") == std::string::npos);
    CHECK(lbody.find("if (__neg) __tmp[__i++] = '-';") != std::string::npos);
    CHECK(lbody.find("__r.size = 0") == std::string::npos);
    CHECK(lbody.find("__r.size = static_cast<std::ptrdiff_t>(__put)") == std::string::npos);
    CHECK(lbody.find("The unwritten tail was not stored.") != std::string::npos);
    CHECK(lbody.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);
}

TEST_CASE("cxx format model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxformat-doctest";
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

TEST_CASE("cxx format model: a short buffer does not report size 0 for a non-zero int") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "format_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "full size=2 d0=4 d1=2"));
    CHECK(has_line(b, "tiny size=2 d0=4 rest=x"));
    CHECK_FALSE(has_line(b, "tiny size=0 d0=4 rest=x"));
    bool saw_tiny = false;
    for (const auto& line : b) {
        if (line.rfind("tiny ", 0) != 0) continue;
        saw_tiny = true;
        CHECK(line.find("size=0") == std::string::npos);
        CHECK(line.find("size=2") != std::string::npos);
    }
    CHECK(saw_tiny);
    CHECK(has_line(b, "zeron size=2 untouched=x"));
    CHECK(has_line(b, "null0 size=2"));
    CHECK(has_line(b, "neg size=2 d0=- d1=3"));
    CHECK(has_line(b, "min size=11 text=-2147483648"));
    CHECK(has_line(b, "llfull size=2 d0=4 d1=2"));
    CHECK(has_line(b, "lltiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "llzero size=2 untouched=x"));
    CHECK(has_line(b, "llnull0 size=2"));
    CHECK(has_line(b, "llneg size=2 d0=- d1=3"));
    CHECK(has_line(b, "llnz size=1 d0=7"));
    CHECK(has_line(b, "llcut size=20 c0=- c1=9 c2=2 c3=2 rest=x"));
    CHECK(has_line(b, "llmin size=20 text=-9223372036854775808"));
    CHECK(has_line(b, "llmax size=19 text=9223372036854775807"));
    for (const char* prefix : {"lltiny ", "llcut ", "llnz "}) {
        bool saw = false;
        for (const auto& line : b) {
            if (line.rfind(prefix, 0) != 0) continue;
            saw = true;
            CHECK(line.find("size=0") == std::string::npos);
        }
        CHECK(saw);
    }

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "null"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(both.find("null") != std::string::npos);
    CHECK(out.find("null size=") == std::string::npos);

    std::string ll_out, ll_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullll"}, &ll_out, &ll_err));
    const std::string ll_both = ll_out + ll_err;
    CHECK(ll_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ll_both.find("null") != std::string::npos);
    CHECK(ll_out.find("nullll size=") == std::string::npos);

    std::string bad_out, bad_err;
    CHECK_FALSE(run_cmd({mod.string(), "bad"}, &bad_out, &bad_err));
    const std::string bad_both = bad_out + bad_err;
    CHECK(bad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(bad_both.find("format_to_n model accepts only {}") != std::string::npos);
    CHECK(bad_out.find("bad size=") == std::string::npos);
}
