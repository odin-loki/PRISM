// The <cmath> model header (src/prism/pir/models/cxx/cmath) is a file on the
// include path, the same way tests/cpp/test_cxx_list.cpp includes <list>.
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources. The trace is tests/cxx_models/cmath_trace.cpp.
//
// Header text: a negative sqrt is a __prism_check before any value.
// With clang++, fabs(-2) is 2. isfinite(0) is true, isnan(0) is false, and
// signbit of a negative value is true. Those four classify; they are not a
// proof of a numeric identity. copysign(0, negative) is a negative zero.
// fmin of a NaN and a number is the number. Two NaNs from fmin or fmax are
// a NaN. Those three are not PROVED and not CLEAN. floor(1.5) is 1 and
// floor(-1.5) is -2. ceil(1.5) is 2 and ceil(-1.5) is -1. trunc(1.5) is 1
// and trunc(-1.5) is -1. A NaN stays a NaN and an infinity stays that
// infinity. floor, ceil, and trunc are not PROVED and not CLEAN.

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

fs::path cmath_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "cmath"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct CmathCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

CmathCtx& ctx() {
    static CmathCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <cmath>\n"
                            "int main() { return std::fabs(-2.0) == 2.0 ? 0 : 1; }\n";
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
    fs::path out = tmp / (with_models ? "cmath_trace.model" : "cmath_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "cmath_trace.cpp").string());
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

std::size_t count_of(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos; at += needle.size()) ++n;
    return n;
}

std::string brace_body(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    if (at == std::string::npos) return {};
    const auto open = src.find('{', at);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(open, i - open + 1);
        }
    }
    return {};
}

}  // namespace

TEST_CASE("cxx cmath model: negative sqrt is checked before a value is produced") {
    const fs::path header = cmath_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_CMATH") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("#include <complex>") == std::string::npos);
    {
        std::istringstream lines(src);
        std::string line;
        while (std::getline(lines, line)) {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            CHECK(line.compare(i, 8, "#include") != 0);
        }
    }
    CHECK(src.find("not a proof of numeric accuracy") != std::string::npos);
    CHECK(src.find("not a proof of a numeric identity") != std::string::npos);
    CHECK(src.find("#define _MATH_H 1") != std::string::npos);
    CHECK(src.find("#include <math.h>") == std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("double sin(") == std::string::npos);
    CHECK(src.find("double cos(") == std::string::npos);
    CHECK(src.find("double tan(") == std::string::npos);
    CHECK(src.find("double exp(") == std::string::npos);
    CHECK(src.find("double log(") == std::string::npos);
    CHECK(src.find("double pow(") == std::string::npos);
    CHECK(src.find("constexpr bool isfinite(double __x) { return __builtin_isfinite(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool isfinite(float __x) { return __builtin_isfinite(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool isfinite(long double __x) { return __builtin_isfinite(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool isinf(double __x) { return __builtin_isinf(__x); }") != std::string::npos);
    CHECK(src.find("constexpr bool isinf(float __x) { return __builtin_isinf(__x); }") != std::string::npos);
    CHECK(src.find("constexpr bool isinf(long double __x) { return __builtin_isinf(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool isnan(double __x) { return __builtin_isnan(__x); }") != std::string::npos);
    CHECK(src.find("constexpr bool isnan(float __x) { return __builtin_isnan(__x); }") != std::string::npos);
    CHECK(src.find("constexpr bool isnan(long double __x) { return __builtin_isnan(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool signbit(double __x) { return __builtin_signbit(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool signbit(float __x) { return __builtin_signbitf(__x); }") !=
          std::string::npos);
    CHECK(src.find("constexpr bool signbit(long double __x) { return __builtin_signbitl(__x); }") !=
          std::string::npos);

    const auto fn = src.find("inline double sqrt(double __x)");
    REQUIRE(fn != std::string::npos);
    const auto next = src.find("inline float sqrt(float __x)", fn);
    REQUIRE(next != std::string::npos);
    const std::string body = src.substr(fn, next - fn);
    const auto check = body.find("__prism_check");
    const auto trap = body.find("__builtin_trap");
    const auto zero = body.find("return 0.0");
    const auto real = body.find("__builtin_sqrt(");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(zero != std::string::npos);
    REQUIRE(real != std::string::npos);
    CHECK(check < trap);
    CHECK(check < zero);
    CHECK(check < real);
    CHECK(body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(body.find("sqrt of a negative value") != std::string::npos);
    CHECK(body.find("nan") == std::string::npos);
    CHECK(body.find("NaN") == std::string::npos);

    const auto neg = body.find("if (__x < 0.0)");
    REQUIRE(neg != std::string::npos);
    REQUIRE(check < neg);
    const std::string neg_body = body.substr(neg, zero - neg);
    CHECK(neg_body.find("__builtin_sqrt") == std::string::npos);
    CHECK(neg_body.find("return") == std::string::npos);

    const auto fabs = src.find("double fabs(double __x)");
    REQUIRE(fabs != std::string::npos);
    const auto fabs_end = src.find("float fabs(float __x)", fabs);
    REQUIRE(fabs_end != std::string::npos);
    const std::string fb = src.substr(fabs, fabs_end - fabs);
    CHECK(fb.find("return __builtin_fabs(__x)") != std::string::npos);
    CHECK(fb.find("return 0") == std::string::npos);

    CHECK(src.find("copysign, fmin, and fmax are not PROVED and not CLEAN.") != std::string::npos);
    CHECK(src.find("floor, ceil, and trunc are not PROVED and not CLEAN.") != std::string::npos);
    CHECK(src.find("A NaN stays a NaN.") != std::string::npos);
    CHECK(src.find("does not return 0 in place of a NaN") != std::string::npos);
    CHECK(src.find("An infinity stays that infinity.") != std::string::npos);
    CHECK(count_of(src, "PROVED") == 2);
    CHECK(count_of(src, "CLEAN") == 2);
    CHECK(src.find("negative zero, not a positive") != std::string::npos);
    CHECK(src.find("Zero is not a stand-in") != std::string::npos);
    CHECK(src.find("copysign(int") == std::string::npos);
    CHECK(src.find("fmin(int") == std::string::npos);
    CHECK(src.find("fmax(int") == std::string::npos);
    CHECK(src.find("int copysign(") == std::string::npos);
    CHECK(src.find("int fmin(") == std::string::npos);
    CHECK(src.find("int fmax(") == std::string::npos);

    for (const char* sig : {"constexpr double copysign(double __x, double __y)",
                            "constexpr float copysign(float __x, float __y)",
                            "constexpr long double copysign(long double __x, long double __y)"}) {
        const std::string fn_body = brace_body(src, sig);
        REQUIRE_FALSE(fn_body.empty());
        CHECK(fn_body.find("fabs(__x)") != std::string::npos);
        CHECK(fn_body.find("if (signbit(__y)) return -__m;") != std::string::npos);
        CHECK(fn_body.find("return __m;") != std::string::npos);
        CHECK(fn_body.find("return 0") == std::string::npos);
        CHECK(fn_body.find("0.0") == std::string::npos);
    }
    for (const char* sig : {"constexpr double fmin(double __x, double __y)",
                            "constexpr float fmin(float __x, float __y)",
                            "constexpr long double fmin(long double __x, long double __y)"}) {
        const std::string fn_body = brace_body(src, sig);
        REQUIRE_FALSE(fn_body.empty());
        CHECK(fn_body.find("if (isnan(__x)) return __y;") != std::string::npos);
        CHECK(fn_body.find("if (isnan(__y)) return __x;") != std::string::npos);
        CHECK(fn_body.find("return __x < __y ? __x : __y;") != std::string::npos);
        CHECK(fn_body.find("return 0") == std::string::npos);
        CHECK(fn_body.find("0.0") == std::string::npos);
    }
    for (const char* sig : {"constexpr double fmax(double __x, double __y)",
                            "constexpr float fmax(float __x, float __y)",
                            "constexpr long double fmax(long double __x, long double __y)"}) {
        const std::string fn_body = brace_body(src, sig);
        REQUIRE_FALSE(fn_body.empty());
        CHECK(fn_body.find("if (isnan(__x)) return __y;") != std::string::npos);
        CHECK(fn_body.find("if (isnan(__y)) return __x;") != std::string::npos);
        CHECK(fn_body.find("return __x > __y ? __x : __y;") != std::string::npos);
        CHECK(fn_body.find("return 0") == std::string::npos);
        CHECK(fn_body.find("0.0") == std::string::npos);
    }

    CHECK(src.find("#undef floor") != std::string::npos);
    CHECK(src.find("#undef ceil") != std::string::npos);
    CHECK(src.find("#undef trunc") != std::string::npos);
    CHECK(src.find("floor(int") == std::string::npos);
    CHECK(src.find("ceil(int") == std::string::npos);
    CHECK(src.find("trunc(int") == std::string::npos);
    CHECK(src.find("int floor(") == std::string::npos);
    CHECK(src.find("int ceil(") == std::string::npos);
    CHECK(src.find("int trunc(") == std::string::npos);
    for (const char* line : {"constexpr double floor(double __x) { return __builtin_floor(__x); }",
                             "constexpr float floor(float __x) { return __builtin_floorf(__x); }",
                             "constexpr long double floor(long double __x) { return __builtin_floorl(__x); }",
                             "constexpr double ceil(double __x) { return __builtin_ceil(__x); }",
                             "constexpr float ceil(float __x) { return __builtin_ceilf(__x); }",
                             "constexpr long double ceil(long double __x) { return __builtin_ceill(__x); }",
                             "constexpr double trunc(double __x) { return __builtin_trunc(__x); }",
                             "constexpr float trunc(float __x) { return __builtin_truncf(__x); }",
                             "constexpr long double trunc(long double __x) { return __builtin_truncl(__x); }"}) {
        CHECK(src.find(line) != std::string::npos);
        const std::string fn_body = brace_body(src, line);
        REQUIRE_FALSE(fn_body.empty());
        CHECK(fn_body.find("return 0") == std::string::npos);
        CHECK(fn_body.find("0.0") == std::string::npos);
    }
}

TEST_CASE("cxx cmath model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxcmath-doctest";
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

TEST_CASE("cxx cmath model: fabs(-2) is 2 when the model header is included") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "cmath_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(cmath_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "sqrt0=0"));
    CHECK(has_line(b, "sqrt4=2"));
    CHECK(has_line(b, "fabs2=2"));
    CHECK(has_line(b, "fabs25=1"));
    CHECK(has_line(b, "abs25=1"));
    CHECK(has_line(b, "fin0=1"));
    CHECK(has_line(b, "fin0f=1"));
    CHECK(has_line(b, "fin0l=1"));
    CHECK(has_line(b, "nan0=0"));
    CHECK(has_line(b, "nan0f=0"));
    CHECK(has_line(b, "nan0l=0"));
    CHECK(has_line(b, "inf0=0"));
    CHECK(has_line(b, "inf0f=0"));
    CHECK(has_line(b, "inf0l=0"));
    CHECK(has_line(b, "sgn2=1"));
    CHECK(has_line(b, "sgnf=1"));
    CHECK(has_line(b, "sgnl=1"));
    CHECK(has_line(b, "cp25=1"));
    CHECK(has_line(b, "cpn=1"));
    CHECK(has_line(b, "cp0=1"));
    CHECK(has_line(b, "cp0p=1"));
    CHECK(has_line(b, "cpmag=1"));
    CHECK(has_line(b, "cp0f=1"));
    CHECK(has_line(b, "cpnf=1"));
    CHECK(has_line(b, "cp0l=1"));
    CHECK(has_line(b, "cpnl=1"));
    CHECK(has_line(b, "fmin13=1"));
    CHECK(has_line(b, "fmax13=1"));
    CHECK(has_line(b, "fminn=1"));
    CHECK(has_line(b, "fminnan=1"));
    CHECK(has_line(b, "fminnan2=1"));
    CHECK(has_line(b, "fmaxnan=1"));
    CHECK(has_line(b, "fmaxnan2=1"));
    CHECK(has_line(b, "fminnn=1"));
    CHECK(has_line(b, "fmaxnn=1"));
    CHECK(has_line(b, "fminf=1"));
    CHECK(has_line(b, "fmaxf=1"));
    CHECK(has_line(b, "fminnf=1"));
    CHECK(has_line(b, "fmaxnnf=1"));
    CHECK(has_line(b, "fminl=1"));
    CHECK(has_line(b, "fmaxl=1"));
    CHECK(has_line(b, "fminnl=1"));
    CHECK(has_line(b, "fmaxnnl=1"));
    CHECK(has_line(b, "fl15=1"));
    CHECK(has_line(b, "fln15=1"));
    CHECK(has_line(b, "ce15=1"));
    CHECK(has_line(b, "cen15=1"));
    CHECK(has_line(b, "tr15=1"));
    CHECK(has_line(b, "trn15=1"));
    CHECK(has_line(b, "fl2=1"));
    CHECK(has_line(b, "fln2=1"));
    CHECK(has_line(b, "ce2=1"));
    CHECK(has_line(b, "cen2=1"));
    CHECK(has_line(b, "tr2=1"));
    CHECK(has_line(b, "trn2=1"));
    CHECK(has_line(b, "flnan=1"));
    CHECK(has_line(b, "cenan=1"));
    CHECK(has_line(b, "trnan=1"));
    CHECK(has_line(b, "flpinf=1"));
    CHECK(has_line(b, "flninf=1"));
    CHECK(has_line(b, "cepinf=1"));
    CHECK(has_line(b, "ceninf=1"));
    CHECK(has_line(b, "trpinf=1"));
    CHECK(has_line(b, "trninf=1"));
    CHECK(has_line(b, "flmz=1"));
    CHECK(has_line(b, "cemz=1"));
    CHECK(has_line(b, "trmz=1"));
    CHECK(has_line(b, "flpz=1"));
    CHECK(has_line(b, "fl15f=1"));
    CHECK(has_line(b, "fln15f=1"));
    CHECK(has_line(b, "ce15f=1"));
    CHECK(has_line(b, "cen15f=1"));
    CHECK(has_line(b, "tr15f=1"));
    CHECK(has_line(b, "trn15f=1"));
    CHECK(has_line(b, "flnanf=1"));
    CHECK(has_line(b, "cenanf=1"));
    CHECK(has_line(b, "trnanf=1"));
    CHECK(has_line(b, "flpinff=1"));
    CHECK(has_line(b, "flninff=1"));
    CHECK(has_line(b, "cepinff=1"));
    CHECK(has_line(b, "ceninff=1"));
    CHECK(has_line(b, "trpinff=1"));
    CHECK(has_line(b, "trninff=1"));
    CHECK(has_line(b, "flmzf=1"));
    CHECK(has_line(b, "cemzf=1"));
    CHECK(has_line(b, "trmzf=1"));
    CHECK(has_line(b, "fl15l=1"));
    CHECK(has_line(b, "fln15l=1"));
    CHECK(has_line(b, "ce15l=1"));
    CHECK(has_line(b, "cen15l=1"));
    CHECK(has_line(b, "tr15l=1"));
    CHECK(has_line(b, "trn15l=1"));
    CHECK(has_line(b, "flnanl=1"));
    CHECK(has_line(b, "cenanl=1"));
    CHECK(has_line(b, "trnanl=1"));
    CHECK(has_line(b, "flpinfl=1"));
    CHECK(has_line(b, "flninfl=1"));
    CHECK(has_line(b, "cepinfl=1"));
    CHECK(has_line(b, "ceninfl=1"));
    CHECK(has_line(b, "trpinfl=1"));
    CHECK(has_line(b, "trninfl=1"));
    CHECK(has_line(b, "flmzl=1"));
    CHECK(has_line(b, "cemzl=1"));
    CHECK(has_line(b, "trmzl=1"));

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "neg"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("sqrt of a negative value") != std::string::npos);
    CHECK(out.find("value=") == std::string::npos);
}
