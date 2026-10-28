// Differential test of the partial <numeric> model
// (src/prism/pir/models/cxx/numeric). The header is a file, so
// `#include <numeric>` is not EISDIR. CMake's prism_tests source list is
// explicit (not a glob).
//
// Midpoint of a large positive pair is not negative. gcd(0, 0) is 0.
// The header text does not use the expression a + b as the midpoint.
// lcm(0, x), lcm(x, 0), and lcm(0, 0) are 0. A product that does not fit
// is __prism_check then __builtin_trap, and that path does not return.

#include <doctest/doctest.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "numeric"; }

struct NumCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

NumCtx& ctx() {
    static NumCtx c;
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
    std::ofstream(probe) << "#include <numeric>\n"
                            "#include <climits>\n"
                            "int main() {\n"
                            "  if (std::midpoint(2, 4) != 3) return 1;\n"
                            "  if (std::gcd(0, 0) != 0) return 2;\n"
                            "  if (std::midpoint(INT_MAX, INT_MAX) != INT_MAX) return 3;\n"
                            "  if (std::lcm(0, 0) != 0 || std::lcm(0, 15) != 0 || std::lcm(15, 0) != 0) return 4;\n"
                            "  if (std::lcm(12, 18) != 36 || std::lcm(-4, 6) != 12) return 5;\n"
                            "  return 0;\n"
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
    fs::path out = tmp / (with_models ? "numeric_trace.model" : "numeric_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "numeric_trace.cpp").string());
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

bool syntax_only(const std::string& cxx, const fs::path& src, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only", "-w"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("cxx numeric model: header is a file and midpoint does not add the arguments") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_NUMERIC") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    // The midpoint is not the sum of the two arguments.
    CHECK(text.find("a + b") == std::string::npos);
    CHECK(text.find("a+b") == std::string::npos);
    CHECK(text.find("__a + __b") == std::string::npos);
    CHECK(text.find("__a+__b") == std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("lcm is not declared") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    const auto lcm_at = text.find("\nlcm(");
    REQUIRE(lcm_at != std::string::npos);
    const std::string lcm_body = text.substr(lcm_at);
    const auto zero_ret = lcm_body.find("if (__um == 0 || __un == 0) return static_cast<_Ct>(0);");
    const auto quot = lcm_body.find("const _Up __q = __um / __g;");
    const auto check = lcm_body.find(
        "__prism_check(0, \"FUNC-CONTRACT\", \"std::lcm product does not fit in the return type\")");
    const auto trap = lcm_body.find("__builtin_trap()");
    const auto prod = lcm_body.find("return static_cast<_Ct>(__q * __un);");
    REQUIRE(zero_ret != std::string::npos);
    REQUIRE(quot != std::string::npos);
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(prod != std::string::npos);
    CHECK(lcm_body.find("gcd(__um, __un)") != std::string::npos);
    CHECK(zero_ret < quot);
    CHECK(quot < check);
    CHECK(check < trap);
    CHECK(trap < prod);
    const std::string arm = lcm_body.substr(check, trap - check);
    CHECK(arm.find("__q * __un") == std::string::npos);
    CHECK(arm.find("static_cast<_Ct>(0)") == std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
    const std::string after_trap = lcm_body.substr(trap, prod - trap);
    CHECK(after_trap.find("return") == std::string::npos);
    const auto mid = text.find("midpoint(");
    REQUIRE(mid != std::string::npos);
    const auto gcd_at = text.find("\ngcd(", mid);
    REQUIRE(gcd_at != std::string::npos);
    const std::string body = text.substr(mid, gcd_at - mid);
    CHECK(body.find("a + b") == std::string::npos);
    CHECK(body.find(" + ") != std::string::npos);
    CHECK(body.find("make_unsigned_t") != std::string::npos);
}

TEST_CASE("cxx numeric model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxnumeric-doctest";
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

TEST_CASE("cxx numeric model: large midpoint is not negative and gcd(0, 0) is 0") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "numeric_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "hi_neg=0 hi_zero=0 hi_eq=1"));
    CHECK(has_line(b, "near_neg=0 near_zero=0 near=" + std::to_string(INT_MAX - 1)));
    CHECK(has_line(b, "small=3"));
    CHECK(has_line(b, "gcd00=0"));
    CHECK(has_line(b, "gcd=6"));
    CHECK(has_line(b, "gcd0=15"));
    CHECK(has_line(b, "gcd_neg=2"));
    CHECK(has_line(b, "lcm00=0"));
    CHECK(has_line(b, "lcm0=0"));
    CHECK(has_line(b, "lcm_x0=0"));
    CHECK(has_line(b, "lcm=36"));
    CHECK(has_line(b, "lcm_neg=12"));
    CHECK(has_line(b, "lcm_both=12"));
    CHECK(has_line(b, "lcm_max=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "lcm_ll=" + std::to_string(static_cast<long long>(INT_MAX) * 2)));
    CHECK(has_line(b, "lcm_u=36"));
    CHECK(has_line(b, "lcm_mix=24"));

    std::string zero_out;
    REQUIRE(run_cmd({mod.string(), "zero"}, &zero_out));
    CHECK(zero_out.find("zero_min=0 min_zero=0 zero_zero=0") != std::string::npos);

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "overflow"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("std::lcm product does not fit in the return type") != std::string::npos);
    CHECK(out.find("wrapped=") == std::string::npos);

    const fs::path overflow_const = c.tmp / "lcm_overflow_const.cpp";
    std::ofstream(overflow_const) << "#include <numeric>\n"
                                    "#include <climits>\n"
                                    "int main() {\n"
                                    "  constexpr int v = std::lcm(INT_MAX, 2);\n"
                                    "  return v;\n"
                                    "}\n";
    CHECK_FALSE(syntax_only(c.cxx, overflow_const, true));

    const fs::path fits_const = c.tmp / "lcm_fits_const.cpp";
    std::ofstream(fits_const) << "#include <numeric>\n"
                                "#include <climits>\n"
                                "static_assert(std::lcm(0, 0) == 0);\n"
                                "static_assert(std::lcm(0, 15) == 0);\n"
                                "static_assert(std::lcm(15, 0) == 0);\n"
                                "static_assert(std::lcm(12, 18) == 36);\n"
                                "static_assert(std::lcm(-4, 6) == 12);\n"
                                "static_assert(std::lcm(INT_MAX, 1) == INT_MAX);\n"
                                "static_assert(std::lcm(INT_MAX, 2LL) == static_cast<long long>(INT_MAX) * 2);\n";
    CHECK(syntax_only(c.cxx, fits_const, true));
    CHECK(syntax_only(c.cxx, fits_const, false));
}
