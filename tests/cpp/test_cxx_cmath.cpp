// The <cmath> model header (src/prism/pir/models/cxx/cmath) is a file on the
// include path, the same way tests/cpp/test_cxx_list.cpp includes <list>.
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources. The trace is tests/cxx_models/cmath_trace.cpp.
//
// Header text: a negative sqrt is a __prism_check before any value.
// With clang++, fabs(-2) is 2. isfinite(0) is true, isnan(0) is false, and
// signbit of a negative value is true. Those four classify; they are not a
// proof of a numeric identity.

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

}  // namespace

TEST_CASE("cxx cmath model: negative sqrt is checked before a value is produced") {
    const fs::path header = cmath_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
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

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "neg"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("sqrt of a negative value") != std::string::npos);
    CHECK(out.find("value=") == std::string::npos);
}
