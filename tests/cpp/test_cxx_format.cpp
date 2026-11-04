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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <format>\n"
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

// The slice is one format_to_n overload. std::format is declared later in the
// header; these overloads do not call it.
std::string fn_slice(const std::string& text, const std::string& sig) {
    const auto at = text.find(sig);
    REQUIRE(at != std::string::npos);
    auto end = text.find("inline format_to_n_result format_to_n(", at + sig.size());
    const auto session = text.find("\n// ---- Session 4", at);
    if (end == std::string::npos || (session != std::string::npos && session < end)) end = session;
    if (end == std::string::npos) end = text.size();
    return text.substr(at, end - at);
}

void check_n0_returns_out(const std::string& body, const std::string& marker) {
    const auto zero = body.find(marker);
    CHECK(zero != std::string::npos);
    if (zero == std::string::npos) return;
    const auto ret = body.find("return __r;", zero);
    CHECK(ret != std::string::npos);
    if (ret == std::string::npos) return;
    const std::string arm = body.substr(zero, ret - zero);
    CHECK(arm.find("__r.out = __out;") != std::string::npos);
    CHECK(arm.find("__r.out = nullptr") == std::string::npos);
    CHECK(arm.find("__out[") == std::string::npos);
    CHECK(arm.find("__prism_format_write") == std::string::npos);
}

const char* k_int = "format_to_n(char* __outv, std::size_t __n, const char* __fmt, int __v)";
const char* k_ll = "format_to_n(char* __outv, std::size_t __n, const char* __fmt, long long __v)";
const char* k_ull = "format_to_n(char* __out, std::size_t __n, const char* __fmt, unsigned long long __v)";
const char* k_u = "format_to_n(char* __out, std::size_t __n, const char* __fmt, unsigned __v)";
const char* k_bool = "format_to_n(char* __out, std::size_t __n, const char* __fmt, bool __v)";
const char* k_char = "format_to_n(char* __out, std::size_t __n, const char* __fmt, char __v)";
const char* k_schar = "format_to_n(char* __out, std::size_t __n, const char* __fmt, signed char __v)";
const char* k_short = "format_to_n(char* __out, std::size_t __n, const char* __fmt, short __v)";
const char* k_uchar = "format_to_n(char* __out, std::size_t __n, const char* __fmt, unsigned char __v)";
const char* k_ushort = "format_to_n(char* __out, std::size_t __n, const char* __fmt, unsigned short __v)";
const char* k_long = "format_to_n(char* __out, std::size_t __n, const char* __fmt, long __v)";
const char* k_ulong = "format_to_n(char* __out, std::size_t __n, const char* __fmt, unsigned long __v)";

void check_order(const std::string& text) {
    const auto a = text.find(k_int);
    const auto b = text.find(k_ll);
    const auto c = text.find(k_ull);
    const auto d = text.find(k_u);
    const auto e = text.find(k_bool);
    const auto f = text.find(k_char);
    const auto g = text.find(k_schar);
    const auto h = text.find(k_short);
    const auto i = text.find(k_uchar);
    const auto j = text.find(k_ushort);
    const auto k = text.find(k_long);
    const auto l = text.find(k_ulong);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    REQUIRE(c != std::string::npos);
    REQUIRE(d != std::string::npos);
    REQUIRE(e != std::string::npos);
    REQUIRE(f != std::string::npos);
    REQUIRE(g != std::string::npos);
    REQUIRE(h != std::string::npos);
    REQUIRE(i != std::string::npos);
    REQUIRE(j != std::string::npos);
    REQUIRE(k != std::string::npos);
    REQUIRE(l != std::string::npos);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(c < d);
    CHECK(d < e);
    CHECK(e < f);
    CHECK(f < g);
    CHECK(g < h);
    CHECK(h < i);
    CHECK(i < j);
    CHECK(j < k);
    CHECK(k < l);
}

void check_slice_not_format_call(const std::string& body) {
    CHECK(body.find("std::format(") == std::string::npos);
    CHECK(body.find("\nformat(") == std::string::npos);
    CHECK(body.find("\nformat_to(") == std::string::npos);
    CHECK(body.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(body.find("__r.size = 0") == std::string::npos);
    CHECK(body.find("__r.size = static_cast<std::ptrdiff_t>(__put)") == std::string::npos);
}

void check_null_before_write(const std::string& body) {
    const auto brace = body.find('{');
    const auto zero = body.find("if (__n == 0)");
    REQUIRE(brace != std::string::npos);
    REQUIRE(zero != std::string::npos);
    const std::string before_zero = body.substr(brace, zero - brace);
    CHECK(before_zero.find("__out") == std::string::npos);
    CHECK(before_zero.find("__prism_format_write") == std::string::npos);
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("nullptr") == std::string::npos);
    CHECK(zero_arm.find("__out[") == std::string::npos);
    CHECK(zero_arm.find("__prism_format_write") == std::string::npos);
    CHECK(zero_arm.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);

    const auto fmt_trap = body.find("if (!__fmt_ok) __builtin_trap();");
    const auto null_msg = body.find("format_to_n called with a null output pointer");
    REQUIRE(fmt_trap != std::string::npos);
    REQUIRE(null_msg != std::string::npos);
    const auto null_call = body.rfind("__prism_check(", null_msg);
    const auto null_end = body.find(");", null_msg);
    REQUIRE(null_call != std::string::npos);
    REQUIRE(null_end != std::string::npos);
    const std::string null_txt = body.substr(null_call, null_end - null_call);
    CHECK(null_txt.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(null_txt.find("nullptr") != std::string::npos);
    CHECK(null_txt.find("null") != std::string::npos);
    const auto null_trap = body.find("if (__out == nullptr) __builtin_trap();", null_end);
    const auto call = body.find("__prism_format_write(");
    REQUIRE(null_trap != std::string::npos);
    REQUIRE(call != std::string::npos);
    CHECK(fmt_trap < zero);
    CHECK(zero < null_call);
    CHECK(null_call < null_trap);
    CHECK(null_trap < call);
    const std::string null_arm = body.substr(null_call, null_trap - null_call);
    CHECK(null_arm.find("__prism_format_write") == std::string::npos);
    CHECK(null_arm.find("__out[") == std::string::npos);
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
    // session 5: std::format returns std::string
    CHECK(text.find("#include <string>") != std::string::npos);
    CHECK(text.find("#include <string_view>") != std::string::npos);
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

TEST_CASE("cxx format model: unsigned long long shares the writer and has no minus sign") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <memory>") == std::string::npos);
    check_order(text);
    const std::string body = fn_slice(text, k_ull);
    check_slice_not_format_call(body);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("__neg") == std::string::npos);
    CHECK(body.find("static_cast<unsigned long long>(0) -") == std::string::npos);
    CHECK(body.find("unsigned long long __mag = __v;") != std::string::npos);
    CHECK(body.find("__prism_format_write(") != std::string::npos);
    CHECK(body.find("__out[__k]") == std::string::npos);
    CHECK(body.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);
    check_n0_returns_out(body, "if (__n == 0)");
    check_null_before_write(body);
    const auto len_check =
        body.find("__prism_check(0, \"FUNC-CONTRACT\", \"format_to_n length does not fit in size_t\")");
    REQUIRE(len_check != std::string::npos);
    const auto len_trap = body.find("__builtin_trap()", len_check);
    const auto len_else = body.find("else", len_trap);
    const auto len_set = body.find("__len = __nd;", len_else);
    const auto zero = body.find("if (__n == 0)");
    REQUIRE(len_trap != std::string::npos);
    REQUIRE(len_else != std::string::npos);
    REQUIRE(len_set != std::string::npos);
    CHECK(len_check < len_trap);
    CHECK(len_trap < len_else);
    CHECK(len_else < len_set);
    CHECK(len_set < zero);
    const std::string len_arm = body.substr(len_check, len_trap - len_check);
    CHECK(len_arm.find("__prism_format_write") == std::string::npos);
    CHECK(len_arm.find("__out") == std::string::npos);
    CHECK(len_arm.find("__tmp") == std::string::npos);
    CHECK(len_arm.find("return") == std::string::npos);
}

TEST_CASE("cxx format model: unsigned shares the writer and has no minus sign") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <memory>") == std::string::npos);
    check_order(text);
    const std::string body = fn_slice(text, k_u);
    check_slice_not_format_call(body);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("__neg") == std::string::npos);
    CHECK(body.find("static_cast<unsigned>(0) -") == std::string::npos);
    CHECK(body.find("unsigned __mag = __v;") != std::string::npos);
    CHECK(body.find("__prism_format_write(") != std::string::npos);
    CHECK(body.find("__out[__k]") == std::string::npos);
    CHECK(body.find("__out[") == std::string::npos);
    CHECK(body.find("__r.size = static_cast<std::ptrdiff_t>(__len);") != std::string::npos);
    check_n0_returns_out(body, "if (__n == 0)");
    check_null_before_write(body);
    const auto len_check =
        body.find("__prism_check(0, \"FUNC-CONTRACT\", \"format_to_n length does not fit in size_t\")");
    REQUIRE(len_check != std::string::npos);
    const auto len_trap = body.find("__builtin_trap()", len_check);
    const auto len_else = body.find("else", len_trap);
    const auto len_set = body.find("__len = __nd;", len_else);
    const auto rev_store = body.find("__rev[");
    const auto tmp_store = body.find("__tmp[");
    const auto zero = body.find("if (__n == 0)");
    REQUIRE(len_trap != std::string::npos);
    REQUIRE(len_else != std::string::npos);
    REQUIRE(len_set != std::string::npos);
    REQUIRE(rev_store != std::string::npos);
    REQUIRE(tmp_store != std::string::npos);
    CHECK(len_check < len_trap);
    CHECK(len_trap < len_else);
    CHECK(len_else < len_set);
    CHECK(len_set < rev_store);
    CHECK(rev_store < tmp_store);
    CHECK(tmp_store < zero);
    const std::string len_arm = body.substr(len_check, len_trap - len_check);
    CHECK(len_arm.find("__prism_format_write") == std::string::npos);
    CHECK(len_arm.find("__out") == std::string::npos);
    CHECK(len_arm.find("__tmp") == std::string::npos);
    CHECK(len_arm.find("__rev") == std::string::npos);
    CHECK(len_arm.find("return") == std::string::npos);
}

TEST_CASE("cxx format model: bool writes true or false and traps a null output before any write") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <memory>") == std::string::npos);
    check_order(text);
    const std::string body = fn_slice(text, k_bool);
    check_slice_not_format_call(body);
    CHECK(body.find("__out[") == std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("__neg") == std::string::npos);
    CHECK(body.find("__tmp[5]") == std::string::npos);
    CHECK(body.find("__v ? static_cast<std::size_t>(4) : static_cast<std::size_t>(5)") != std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    check_n0_returns_out(body, "if (__n == 0)");
    const auto brace = body.find('{');
    const auto zero = body.find("if (__n == 0)");
    const std::string before_zero = body.substr(brace, zero - brace);
    CHECK(before_zero.find("__tmp[") == std::string::npos);
    const auto zero_ret = body.find("return __r;", zero);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__tmp[") == std::string::npos);
    check_null_before_write(body);
    const auto null_trap = body.find("if (__out == nullptr) __builtin_trap();");
    const auto t0 = body.find("__tmp[0] = 't';");
    const auto t1 = body.find("__tmp[1] = 'r';");
    const auto t2 = body.find("__tmp[2] = 'u';");
    const auto t3 = body.find("__tmp[3] = 'e';");
    const auto f0 = body.find("__tmp[0] = 'f';");
    const auto f1 = body.find("__tmp[1] = 'a';");
    const auto f2 = body.find("__tmp[2] = 'l';");
    const auto f3 = body.find("__tmp[3] = 's';");
    const auto f4 = body.find("__tmp[4] = 'e';");
    const auto call = body.find("__prism_format_write(");
    REQUIRE(t0 != std::string::npos);
    REQUIRE(f4 != std::string::npos);
    CHECK(null_trap < t0);
    CHECK(t0 < t1);
    CHECK(t1 < t2);
    CHECK(t2 < t3);
    CHECK(t3 < f0);
    CHECK(f0 < f1);
    CHECK(f1 < f2);
    CHECK(f2 < f3);
    CHECK(f3 < f4);
    CHECK(f4 < call);
    CHECK(body.find("__tmp[0] = 't';", t0 + 1) == std::string::npos);
    CHECK(body.find("__tmp[0] = 'f';", f0 + 1) == std::string::npos);
    const auto size_after = body.find("__r.size = static_cast<std::ptrdiff_t>(__len);", call);
    REQUIRE(size_after != std::string::npos);
    CHECK(call < size_after);
}

TEST_CASE("cxx format model: char writes that one character and traps a null output before any write") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find(k_uchar) != std::string::npos);
    check_order(text);
    CHECK(text.find(k_char, text.find(k_char) + 1) == std::string::npos);
    const std::string body = fn_slice(text, k_char);
    check_slice_not_format_call(body);
    CHECK(body.find("__out[") == std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("__neg") == std::string::npos);
    CHECK(body.find("__tmp[1]") == std::string::npos);
    CHECK(body.find("/ 10") == std::string::npos);
    CHECK(body.find("% 10") == std::string::npos);
    CHECK(body.find("static_cast<std::size_t>(1)") != std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    check_n0_returns_out(body, "if (__n == 0)");
    check_null_before_write(body);
    const auto null_trap = body.find("if (__out == nullptr) __builtin_trap();");
    const auto store = body.find("__tmp[0] = __v;");
    const auto call = body.find("__prism_format_write(");
    REQUIRE(store != std::string::npos);
    CHECK(null_trap < store);
    CHECK(store < call);
    CHECK(body.find("__tmp[0] = __v;", store + 1) == std::string::npos);
}

TEST_CASE("cxx format model: short calls the int overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <memory>") == std::string::npos);
    check_order(text);
    CHECK(text.find(k_short, text.find(k_short) + 1) == std::string::npos);
    const std::string body = fn_slice(text, k_short);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("__out[") == std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("__neg") == std::string::npos);
    CHECK(body.find("/ 10") == std::string::npos);
    CHECK(body.find("% 10") == std::string::npos);
    CHECK(body.find("__tmp") == std::string::npos);
    const auto cast = body.find("static_cast<int>(__v)");
    REQUIRE(cast != std::string::npos);
    CHECK(body.find("static_cast<int>(__v)", cast + 1) != std::string::npos);
    const auto brace = body.find('{');
    const auto zero = body.find("if (__n == 0)");
    REQUIRE(brace != std::string::npos);
    REQUIRE(zero != std::string::npos);
    const std::string before_zero = body.substr(brace, zero - brace);
    CHECK(before_zero.find("__out") == std::string::npos);
    CHECK(before_zero.find("static_cast<int>") == std::string::npos);
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("nullptr") == std::string::npos);
    CHECK(zero_arm.find("__out[") == std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))") != std::string::npos);
    const auto after = body.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))", zero_ret);
    REQUIRE(after != std::string::npos);
    CHECK(body.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))", after + 1) == std::string::npos);
}

TEST_CASE("cxx format model: unsigned short calls the unsigned overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    check_order(text);
    CHECK(text.find(k_ushort, text.find(k_ushort) + 1) == std::string::npos);
    const std::string body = fn_slice(text, k_ushort);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("FUNC-CONTRACT") == std::string::npos);
    CHECK(body.find("__out[") == std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    const auto cast = body.find("static_cast<unsigned>(__v)");
    REQUIRE(cast != std::string::npos);
    CHECK(body.find("static_cast<unsigned>(__v)", cast + 1) != std::string::npos);
    const auto zero = body.find("if (__n == 0)");
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero != std::string::npos);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<unsigned>(__v))") != std::string::npos);
    const auto after = body.find("format_to_n(__out, __n, __fmt, static_cast<unsigned>(__v))", zero_ret);
    REQUIRE(after != std::string::npos);
    CHECK(body.find("format_to_n(__out, __n, __fmt, static_cast<unsigned>(__v))", after + 1) == std::string::npos);
}

TEST_CASE("cxx format model: long calls the long long overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    check_order(text);
    CHECK(text.find(k_long, text.find(k_long) + 1) == std::string::npos);
    const std::string body = fn_slice(text, k_long);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("FUNC-CONTRACT") == std::string::npos);
    CHECK(body.find("static_cast<long long>(__v)") != std::string::npos);
    const auto zero = body.find("if (__n == 0)");
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<long long>(__v))") != std::string::npos);
    const auto after = body.find("format_to_n(__out, __n, __fmt, static_cast<long long>(__v))", zero_ret);
    REQUIRE(after != std::string::npos);
    CHECK(body.find("format_to_n(__out, __n, __fmt, static_cast<long long>(__v))", after + 1) == std::string::npos);
}

TEST_CASE("cxx format model: unsigned long calls the unsigned long long overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    check_order(text);
    const std::string body = fn_slice(text, k_ulong);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("static_cast<unsigned long long>(__v)") != std::string::npos);
    const auto zero = body.find("if (__n == 0)");
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<unsigned long long>(__v))") != std::string::npos);
}

TEST_CASE("cxx format model: signed char calls the int overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    check_order(text);
    const std::string body = fn_slice(text, k_schar);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("static_cast<int>(__v)") != std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    CHECK(body.find("% 10") == std::string::npos);
    const auto zero = body.find("if (__n == 0)");
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))") != std::string::npos);
    const auto after = body.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))", zero_ret);
    REQUIRE(after != std::string::npos);
    CHECK(body.find("format_to_n(__out, __n, __fmt, static_cast<int>(__v))", after + 1) == std::string::npos);
}

TEST_CASE("cxx format model: unsigned char calls the unsigned overload and n == 0 returns the caller pointer") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    check_order(text);
    const std::string body = fn_slice(text, k_uchar);
    check_slice_not_format_call(body);
    CHECK(body.find("__prism_format_write") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(body.find("FUNC-CONTRACT") == std::string::npos);
    CHECK(body.find("static_cast<unsigned>(__v)") != std::string::npos);
    CHECK(body.find("'-'") == std::string::npos);
    const auto zero = body.find("if (__n == 0)");
    const auto zero_ret = body.find("return __r;", zero);
    REQUIRE(zero_ret != std::string::npos);
    const std::string zero_arm = body.substr(zero, zero_ret - zero);
    CHECK(zero_arm.find("__r.out = __out;") != std::string::npos);
    CHECK(zero_arm.find("format_to_n(__out, __n, __fmt, static_cast<unsigned>(__v))") != std::string::npos);
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
    CHECK(has_line(b, "ullfull size=2 d0=4 d1=2"));
    CHECK(has_line(b, "ulltiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "ullzero size=2 untouched=x same=1"));
    CHECK(has_line(b, "ullnull0 size=2 same=1"));
    CHECK(has_line(b, "ullnz size=1 d0=7"));
    CHECK(has_line(b, "ullz size=1 d0=0"));
    CHECK(has_line(b, "ullwide size=11 c0=1 c1=0 c2=0 c3=0 rest=x"));
    CHECK(has_line(b, "ullcut size=20 c0=1 c1=8 c2=4 c3=4 rest=x"));
    CHECK(has_line(b, "ullmax size=20 text=18446744073709551615"));
    CHECK(has_line(b, "ufull size=2 d0=4 d1=2"));
    CHECK(has_line(b, "utiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "uzero size=2 untouched=x same=1"));
    CHECK(has_line(b, "unull0 size=2 same=1"));
    CHECK(has_line(b, "unz size=1 d0=7"));
    CHECK(has_line(b, "uz size=1 d0=0"));
    CHECK(has_line(b, "uwide size=10 c0=3 c1=0 c2=0 c3=0 rest=x"));
    CHECK(has_line(b, "ucut size=10 c0=4 c1=2 c2=9 c3=4 rest=x"));
    CHECK(has_line(b, "umax size=10 text=4294967295"));
    CHECK(has_line(b, "btrue size=4 c0=t c1=r c2=u c3=e rest=x"));
    CHECK(has_line(b, "bfalse size=5 c0=f c1=a c2=l c3=s c4=e rest=x"));
    CHECK(has_line(b, "btruetiny size=4 d0=t rest=x"));
    CHECK(has_line(b, "bfalsetiny size=5 d0=f rest=x"));
    CHECK(has_line(b, "bzero size=4 untouched=x same=1"));
    CHECK(has_line(b, "bfalse0 size=5 untouched=x same=1"));
    CHECK(has_line(b, "bnull0 size=4 same=1"));
    CHECK(has_line(b, "bfalsenull0 size=5 same=1"));
    CHECK(has_line(b, "bcut size=5 c0=f c1=a c2=l c3=s rest=x"));
    CHECK(has_line(b, "btrue3 size=4 c0=t c1=r c2=u rest=x"));
    CHECK(has_line(b, "cfull size=1 c0=A rest=x adv=1"));
    CHECK(has_line(b, "cQ size=1 c0=Q rest=x"));
    CHECK(has_line(b, "cn1 size=1 c0=A rest=x"));
    CHECK(has_line(b, "czero size=1 untouched=x same=1"));
    CHECK(has_line(b, "cQ0 size=1 untouched=x same=1"));
    CHECK(has_line(b, "cnull0 size=1 same=1"));
    CHECK(has_line(b, "shfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "shtiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "shzero size=2 untouched=x same=1"));
    CHECK(has_line(b, "shnull0 size=2 same=1"));
    CHECK(has_line(b, "shneg size=2 d0=- d1=3"));
    CHECK(has_line(b, "shz size=1 d0=0"));
    CHECK(has_line(b, "shmin size=6 text=-32768"));
    CHECK(has_line(b, "shmax size=5 text=32767"));
    CHECK(has_line(b, "shcut size=6 c0=- c1=3 c2=2 c3=7 rest=x"));
    CHECK(has_line(b, "usfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "ustiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "uszero size=2 untouched=x same=1"));
    CHECK(has_line(b, "usnull0 size=2 same=1"));
    CHECK(has_line(b, "usz size=1 d0=0"));
    CHECK(has_line(b, "usmax size=5 text=65535"));
    CHECK(has_line(b, "uscut size=5 c0=6 c1=5 c2=5 c3=3 rest=x"));
    CHECK(has_line(b, "lgfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "lgtiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "lgzero size=2 untouched=x same=1"));
    CHECK(has_line(b, "lgnull0 size=2 same=1"));
    CHECK(has_line(b, "lgneg size=2 d0=- d1=3"));
    CHECK(has_line(b, "lgz size=1 d0=0"));
    CHECK(has_line(b, "lgmin size=11 text=-2147483648"));
    CHECK(has_line(b, "lgmax size=10 text=2147483647"));
    CHECK(has_line(b, "lgcut size=11 c0=- c1=2 c2=1 c3=4 rest=x"));
    CHECK(has_line(b, "ulfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "ultiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "ulzero size=2 untouched=x same=1"));
    CHECK(has_line(b, "ulnull0 size=2 same=1"));
    CHECK(has_line(b, "ulz size=1 d0=0"));
    CHECK(has_line(b, "ulmax size=10 text=4294967295"));
    CHECK(has_line(b, "ulcut size=10 c0=4 c1=2 c2=9 c3=4 rest=x"));
    CHECK(has_line(b, "scfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "sctiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "sczero size=2 untouched=x same=1"));
    CHECK(has_line(b, "scnull0 size=2 same=1"));
    CHECK(has_line(b, "scneg size=2 d0=- d1=3"));
    CHECK(has_line(b, "scz size=1 d0=0"));
    CHECK(has_line(b, "scmin size=4 text=-128"));
    CHECK(has_line(b, "scmax size=3 text=127"));
    CHECK(has_line(b, "sccut size=4 c0=- c1=1 c2=2 rest=x"));
    CHECK(has_line(b, "ucfull size=2 d0=4 d1=2 rest=x"));
    CHECK(has_line(b, "uctiny size=2 d0=4 rest=x"));
    CHECK(has_line(b, "uczero size=2 untouched=x same=1"));
    CHECK(has_line(b, "ucnull0 size=2 same=1"));
    CHECK(has_line(b, "ucz size=1 d0=0"));
    CHECK(has_line(b, "ucmax size=3 text=255"));
    CHECK(has_line(b, "uccut size=3 c0=2 c1=5 rest=x"));
    CHECK_FALSE(has_line(b, "btrue size=0 c0=t c1=r c2=u c3=e rest=x"));
    CHECK_FALSE(has_line(b, "cfull size=0 c0=A rest=x adv=1"));
    CHECK_FALSE(has_line(b, "shmin size=0 text=-32768"));
    CHECK_FALSE(has_line(b, "ucmax size=0 text=255"));
    for (const char* prefix : {"lltiny ", "llcut ", "llnz ", "ulltiny ", "ullwide ", "ullcut ", "ullnz ", "ullz ",
                               "utiny ", "uwide ", "ucut ", "unz ", "uz ", "btrue ", "bfalse ", "btruetiny ",
                               "bfalsetiny ", "bzero ", "bfalse0 ", "bcut ", "btrue3 ", "cfull ", "cQ ", "cn1 ",
                               "czero ", "cQ0 ", "shfull ", "shtiny ", "shzero ", "shneg ", "shz ", "shmin ",
                               "shmax ", "shcut ", "usfull ", "ustiny ", "uszero ", "usz ", "usmax ", "uscut ",
                               "lgfull ", "lgtiny ", "lgzero ", "lgneg ", "lgz ", "lgmin ", "lgmax ", "lgcut ",
                               "ulfull ", "ultiny ", "ulzero ", "ulz ", "ulmax ", "ulcut ", "scfull ", "sctiny ",
                               "sczero ", "scneg ", "scz ", "scmin ", "scmax ", "sccut ", "ucfull ", "uctiny ",
                               "uczero ", "ucz ", "ucmax ", "uccut "}) {
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

    std::string ull_out, ull_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullull"}, &ull_out, &ull_err));
    const std::string ull_both = ull_out + ull_err;
    CHECK(ull_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ull_both.find("null") != std::string::npos);
    CHECK(ull_out.find("nullull size=") == std::string::npos);

    std::string ubad_out, ubad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badull"}, &ubad_out, &ubad_err));
    const std::string ubad_both = ubad_out + ubad_err;
    CHECK(ubad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ubad_both.find("format_to_n model accepts only {}") != std::string::npos);
    CHECK(ubad_out.find("badull size=") == std::string::npos);

    std::string u_out, u_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullu"}, &u_out, &u_err));
    const std::string u_both = u_out + u_err;
    CHECK(u_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(u_both.find("null") != std::string::npos);
    CHECK(u_out.find("nullu size=") == std::string::npos);

    std::string ubad2_out, ubad2_err;
    CHECK_FALSE(run_cmd({mod.string(), "badu"}, &ubad2_out, &ubad2_err));
    const std::string ubad2_both = ubad2_out + ubad2_err;
    CHECK(ubad2_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ubad2_out.find("badu size=") == std::string::npos);

    std::string b_out, b_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullb"}, &b_out, &b_err));
    const std::string b_both = b_out + b_err;
    CHECK(b_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(b_both.find("null") != std::string::npos);
    CHECK(b_out.find("nullb size=") == std::string::npos);

    std::string bbad_out, bbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badb"}, &bbad_out, &bbad_err));
    const std::string bbad_both = bbad_out + bbad_err;
    CHECK(bbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(bbad_out.find("badb size=") == std::string::npos);

    std::string c_out, c_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullc"}, &c_out, &c_err));
    const std::string c_both = c_out + c_err;
    CHECK(c_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(c_both.find("null") != std::string::npos);
    CHECK(c_out.find("nullc size=") == std::string::npos);

    std::string cbad_out, cbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badc"}, &cbad_out, &cbad_err));
    const std::string cbad_both = cbad_out + cbad_err;
    CHECK(cbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(cbad_out.find("badc size=") == std::string::npos);

    std::string s_out, s_err;
    CHECK_FALSE(run_cmd({mod.string(), "nulls"}, &s_out, &s_err));
    const std::string s_both = s_out + s_err;
    CHECK(s_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(s_both.find("null") != std::string::npos);
    CHECK(s_out.find("nulls size=") == std::string::npos);

    std::string sbad_out, sbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "bads"}, &sbad_out, &sbad_err));
    const std::string sbad_both = sbad_out + sbad_err;
    CHECK(sbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(sbad_out.find("bads size=") == std::string::npos);

    std::string us_out, us_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullus"}, &us_out, &us_err));
    const std::string us_both = us_out + us_err;
    CHECK(us_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(us_both.find("format_to_n called with a null output pointer") != std::string::npos);
    CHECK(us_both.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(us_out.find("nullus size=") == std::string::npos);

    std::string usbad_out, usbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badus"}, &usbad_out, &usbad_err));
    const std::string usbad_both = usbad_out + usbad_err;
    CHECK(usbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(usbad_out.find("badus size=") == std::string::npos);

    std::string lg_out, lg_err;
    CHECK_FALSE(run_cmd({mod.string(), "nulllong"}, &lg_out, &lg_err));
    const std::string lg_both = lg_out + lg_err;
    CHECK(lg_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(lg_both.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(lg_out.find("nulllong size=") == std::string::npos);

    std::string lgbad_out, lgbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badlong"}, &lgbad_out, &lgbad_err));
    const std::string lgbad_both = lgbad_out + lgbad_err;
    CHECK(lgbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(lgbad_out.find("badlong size=") == std::string::npos);

    std::string ul_out, ul_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullulong"}, &ul_out, &ul_err));
    const std::string ul_both = ul_out + ul_err;
    CHECK(ul_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ul_both.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(ul_out.find("nullulong size=") == std::string::npos);

    std::string ulbad_out, ulbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badulong"}, &ulbad_out, &ulbad_err));
    const std::string ulbad_both = ulbad_out + ulbad_err;
    CHECK(ulbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ulbad_out.find("badulong size=") == std::string::npos);

    std::string sc_out, sc_err;
    CHECK_FALSE(run_cmd({mod.string(), "nullsc"}, &sc_out, &sc_err));
    const std::string sc_both = sc_out + sc_err;
    CHECK(sc_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(sc_out.find("nullsc size=") == std::string::npos);

    std::string scbad_out, scbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "badsc"}, &scbad_out, &scbad_err));
    const std::string scbad_both = scbad_out + scbad_err;
    CHECK(scbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(scbad_out.find("badsc size=") == std::string::npos);

    std::string uc_out, uc_err;
    CHECK_FALSE(run_cmd({mod.string(), "nulluc"}, &uc_out, &uc_err));
    const std::string uc_both = uc_out + uc_err;
    CHECK(uc_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(uc_both.find("PTR-NULL-DEREF") == std::string::npos);
    CHECK(uc_out.find("nulluc size=") == std::string::npos);

    std::string ucbad_out, ucbad_err;
    CHECK_FALSE(run_cmd({mod.string(), "baduc"}, &ucbad_out, &ucbad_err));
    const std::string ucbad_both = ucbad_out + ucbad_err;
    CHECK(ucbad_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(ucbad_out.find("baduc size=") == std::string::npos);
}
