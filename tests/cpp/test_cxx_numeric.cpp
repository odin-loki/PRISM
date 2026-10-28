// Differential test of the partial <numeric> model
// (src/prism/pir/models/cxx/numeric). The header is a file, so
// `#include <numeric>` is not EISDIR. CMake's prism_tests source list is
// explicit (not a glob).
//
// Midpoint of a large positive pair is not negative. gcd(0, 0) is 0.
// The header text does not use the expression a + b as the midpoint.
// lcm(0, x), lcm(x, 0), and lcm(0, 0) are 0. A product that does not fit
// is __prism_check then __builtin_trap, and that path does not return.
// std::iota writes value, value + 1, ... into [first, last). An inverted
// range and a next value that does not fit are checks, then a trap, and
// that path does not store a wrapped value.
// std::accumulate returns init plus each element of [first, last). An
// inverted range does not read and does not return init. An empty range
// returns init and does not read. A sum that does not fit is a check, then
// a trap, and that path does not return a wrapped sum.

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
    CHECK(text.find("#include <iterator>") == std::string::npos);
    CHECK(text.find("(a+b)/2") == std::string::npos);
    CHECK(text.find("(a + b) / 2") == std::string::npos);
    CHECK(text.find("partial_sum, iota,") == std::string::npos);
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

TEST_CASE("cxx numeric model: iota traps an inverted range and a next value that does not fit") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <iterator>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    const auto iota_at = text.find("\niota(");
    REQUIRE(iota_at != std::string::npos);
    const std::string iota = text.substr(iota_at);
    const auto inv = iota.find("if (__last < __first)");
    const auto inv_check =
        iota.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::iota last is before first\")");
    const auto empty = iota.find("if (__first == __last) return;");
    const auto store = iota.find("*__first = __value;");
    const auto ovf =
        iota.find("__prism_check(0, \"INT-SIGNED-OVF\", \"std::iota next value does not fit in T\")");
    const auto uovf =
        iota.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::iota next value does not fit in T\")");
    const auto step = iota.find("__value = static_cast<_Tp>(__value + static_cast<_Tp>(1));");
    const auto step2 =
        step == std::string::npos
            ? std::string::npos
            : iota.find("__value = static_cast<_Tp>(__value + static_cast<_Tp>(1));", step + 1);
    REQUIRE(inv != std::string::npos);
    REQUIRE(inv_check != std::string::npos);
    REQUIRE(empty != std::string::npos);
    REQUIRE(store != std::string::npos);
    REQUIRE(ovf != std::string::npos);
    REQUIRE(uovf != std::string::npos);
    REQUIRE(step != std::string::npos);
    REQUIRE(step2 != std::string::npos);
    CHECK(inv < inv_check);
    CHECK(inv_check < empty);
    CHECK(empty < store);
    CHECK(store < ovf);
    CHECK(ovf < uovf);
    const auto inv_trap = iota.find("__builtin_trap()", inv_check);
    REQUIRE(inv_trap != std::string::npos);
    CHECK(inv_trap < empty);
    const std::string inv_arm = iota.substr(inv_check, inv_trap - inv_check);
    CHECK(inv_arm.find("*__first") == std::string::npos);
    CHECK(inv_arm.find("__value +") == std::string::npos);
    CHECK(inv_arm.find("return") == std::string::npos);
    const auto ovf_trap = iota.find("__builtin_trap()", ovf);
    REQUIRE(ovf_trap != std::string::npos);
    CHECK(ovf_trap < step);
    CHECK(step < uovf);
    const std::string ovf_arm = iota.substr(ovf, ovf_trap - ovf);
    CHECK(ovf_arm.find("__value +") == std::string::npos);
    CHECK(ovf_arm.find("*__first") == std::string::npos);
    const auto u_trap = iota.find("__builtin_trap()", uovf);
    REQUIRE(u_trap != std::string::npos);
    CHECK(u_trap < step2);
    const std::string u_arm = iota.substr(uovf, u_trap - uovf);
    CHECK(u_arm.find("__value +") == std::string::npos);
    CHECK(u_arm.find("*__first") == std::string::npos);
    CHECK(iota.find("++__value;") != std::string::npos);
    const auto mid = text.find("midpoint(");
    const auto gcd_at = text.find("\ngcd(", mid);
    REQUIRE(mid != std::string::npos);
    REQUIRE(gcd_at != std::string::npos);
    const std::string mid_body = text.substr(mid, gcd_at - mid);
    CHECK(mid_body.find("a + b") == std::string::npos);
    CHECK(mid_body.find("a+b") == std::string::npos);
    CHECK(text.find("\ngcd(") != std::string::npos);
    CHECK(text.find("while (__un != 0)") != std::string::npos);
}

TEST_CASE("cxx numeric model: accumulate traps an inverted range and a sum that does not fit") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include <iterator>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("#include <algorithm>") == std::string::npos);
    CHECK(text.find("\nreduce(") == std::string::npos);
    CHECK(text.find("\ntransform_reduce(") == std::string::npos);
    CHECK(text.find("\ninner_product(") == std::string::npos);
    CHECK(text.find("\nadjacent_difference(") == std::string::npos);
    CHECK(text.find("\npartial_sum(") == std::string::npos);
    CHECK(text.find("\nexclusive_scan(") == std::string::npos);
    CHECK(text.find("naming accumulate") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    const auto acc_at = text.find("\naccumulate(");
    REQUIRE(acc_at != std::string::npos);
    const std::string acc = text.substr(acc_at);
    const auto inv = acc.find("if (__last < __first)");
    const auto inv_check =
        acc.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::accumulate last is before first\")");
    const auto empty = acc.find("if (__first == __last) return __init;");
    const auto read = acc.find("*__first");
    const auto ovf =
        acc.find("__prism_check(0, \"INT-SIGNED-OVF\", \"std::accumulate sum does not fit in T\")");
    const auto uovf =
        acc.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::accumulate sum does not fit in T\")");
    const auto step = acc.find("__init = static_cast<_Tp>(__init + __elem);");
    const auto step2 = step == std::string::npos
                           ? std::string::npos
                           : acc.find("__init = static_cast<_Tp>(__init + __elem);", step + 1);
    REQUIRE(inv != std::string::npos);
    REQUIRE(inv_check != std::string::npos);
    REQUIRE(empty != std::string::npos);
    REQUIRE(read != std::string::npos);
    REQUIRE(ovf != std::string::npos);
    REQUIRE(uovf != std::string::npos);
    REQUIRE(step != std::string::npos);
    REQUIRE(step2 != std::string::npos);
    CHECK(inv < inv_check);
    CHECK(inv_check < empty);
    CHECK(empty < read);
    CHECK(read < ovf);
    CHECK(ovf < uovf);
    const auto inv_trap = acc.find("__builtin_trap()", inv_check);
    REQUIRE(inv_trap != std::string::npos);
    CHECK(inv_trap < empty);
    const std::string inv_arm = acc.substr(inv_check, inv_trap - inv_check);
    CHECK(inv_arm.find("*__first") == std::string::npos);
    CHECK(inv_arm.find("__elem") == std::string::npos);
    CHECK(inv_arm.find("__init +") == std::string::npos);
    CHECK(inv_arm.find("return") == std::string::npos);
    const auto ovf_trap = acc.find("__builtin_trap()", ovf);
    REQUIRE(ovf_trap != std::string::npos);
    CHECK(ovf_trap < step);
    CHECK(step < uovf);
    const auto ovf_else = acc.find("else", ovf_trap);
    REQUIRE(ovf_else != std::string::npos);
    CHECK(ovf_else < step);
    const std::string ovf_arm = acc.substr(ovf, ovf_trap - ovf);
    CHECK(ovf_arm.find("__init +") == std::string::npos);
    CHECK(ovf_arm.find("return") == std::string::npos);
    const auto u_trap = acc.find("__builtin_trap()", uovf);
    REQUIRE(u_trap != std::string::npos);
    CHECK(u_trap < step2);
    const auto u_else = acc.find("else", u_trap);
    REQUIRE(u_else != std::string::npos);
    CHECK(u_else < step2);
    const std::string u_arm = acc.substr(uovf, u_trap - uovf);
    CHECK(u_arm.find("__init +") == std::string::npos);
    CHECK(u_arm.find("return") == std::string::npos);
    const auto advance = acc.find("++__first");
    REQUIRE(advance != std::string::npos);
    CHECK(step2 < advance);
    CHECK(text.find("while (__un != 0)") != std::string::npos);
    CHECK(text.find("\niota(") != std::string::npos);
    CHECK(text.find("\nlcm(") != std::string::npos);
    CHECK(text.find("\nmidpoint(") != std::string::npos);
    const auto mid = text.find("midpoint(");
    const auto gcd_at = text.find("\ngcd(", mid);
    REQUIRE(mid != std::string::npos);
    REQUIRE(gcd_at != std::string::npos);
    const std::string mid_body = text.substr(mid, gcd_at - mid);
    CHECK(mid_body.find("a + b") == std::string::npos);
    CHECK(mid_body.find("a+b") == std::string::npos);
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
    CHECK(has_line(b, "iota=0,1,2,3"));
    CHECK(has_line(b, "iota_empty=7"));
    CHECK(has_line(b, "iota_neg=-1,0,1"));
    CHECK(has_line(b, "iota_min=" + std::to_string(INT_MIN) + "," + std::to_string(INT_MIN + 1) + "," +
                          std::to_string(INT_MIN + 2)));
    CHECK(has_line(b, "iota_near=" + std::to_string(INT_MAX - 2) + "," + std::to_string(INT_MAX - 1)));
    CHECK(has_line(b, "iota_u=1,2,3"));
    CHECK(has_line(b, "iota_unear=" + std::to_string(UINT_MAX - 2u) + "," + std::to_string(UINT_MAX - 1u)));
    CHECK(has_line(b, "iota_umax=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "iota_one=41"));
    CHECK(has_line(b, "iota_sc=" + std::to_string(SCHAR_MAX - 2) + "," + std::to_string(SCHAR_MAX - 1)));
    CHECK(has_line(b, "iota_uc=" + std::to_string(UCHAR_MAX - 1) + "," + std::to_string(UCHAR_MAX)));
    CHECK(has_line(b, "iota_f=0,1,2"));
    CHECK(has_line(b, "acc=10"));
    CHECK(has_line(b, "acc_empty=3,7"));
    CHECK(has_line(b, "acc_neg=0"));
    CHECK(has_line(b, "acc_min=" + std::to_string(INT_MIN + 2)));
    CHECK(has_line(b, "acc_near=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "acc_hi=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "acc_imin=" + std::to_string(INT_MIN)));
    CHECK(has_line(b, "acc_u=6"));
    CHECK(has_line(b, "acc_unear=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "acc_umax=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "acc_uzero=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "acc_one=41"));
    CHECK(has_line(b, "acc_sc=" + std::to_string(SCHAR_MAX)));
    CHECK(has_line(b, "acc_uc=" + std::to_string(UCHAR_MAX)));
    CHECK(has_line(b, "acc_f=2"));
    CHECK(has_line(b, "acc_b=1"));

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

    std::string hi_out;
    REQUIRE(run_cmd({mod.string(), "iota-max1"}, &hi_out));
    CHECK(hi_out.find("iota_hi=" + std::to_string(INT_MAX)) != std::string::npos);

    std::string iout, ierr;
    CHECK_FALSE(run_cmd({mod.string(), "iota-ovf"}, &iout, &ierr));
    std::string iboth = iout + ierr;
    CHECK(iboth.find("CHECK INT-SIGNED-OVF:") != std::string::npos);
    CHECK(iboth.find("std::iota next value does not fit in T") != std::string::npos);
    CHECK(iout.find("wrapped=") == std::string::npos);

    CHECK_FALSE(run_cmd({mod.string(), "iota-llovf"}, &iout, &ierr));
    iboth = iout + ierr;
    CHECK(iboth.find("CHECK INT-SIGNED-OVF:") != std::string::npos);
    CHECK(iboth.find("std::iota next value does not fit in T") != std::string::npos);
    CHECK(iout.find("wrapped=") == std::string::npos);

    CHECK_FALSE(run_cmd({mod.string(), "iota-uovf"}, &iout, &ierr));
    iboth = iout + ierr;
    CHECK(iboth.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(iboth.find("std::iota next value does not fit in T") != std::string::npos);
    CHECK(iout.find("wrapped=") == std::string::npos);

    CHECK_FALSE(run_cmd({mod.string(), "iota-inv"}, &iout, &ierr));
    iboth = iout + ierr;
    CHECK(iboth.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(iboth.find("std::iota last is before first") != std::string::npos);
    CHECK(iout.find("wrapped=") == std::string::npos);

    const fs::path iota_mem = c.tmp / "iota_mem.cpp";
    std::ofstream(iota_mem) << "#include <climits>\n"
                               "#include <cstdio>\n"
                               "#include <cstdlib>\n"
                               "#include <cstring>\n"
                               "#include <numeric>\n"
                               "static int gs[2] = {4, 5};\n"
                               "static unsigned gu[2] = {4u, 5u};\n"
                               "static long long gll[2] = {4, 5};\n"
                               "static signed char gsc[2] = {1, 1};\n"
                               "static int mode = 0;\n"
                               "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
                               "  if (!ok) {\n"
                               "    std::fprintf(stderr, \"CHECK %s: %s\\n\", cls ? cls : \"\", msg ? msg : \"\");\n"
                               "    if (mode == 1)\n"
                               "      std::fprintf(stderr, \"mem=%u,%u\\n\", gu[0], gu[1]);\n"
                               "    else if (mode == 2)\n"
                               "      std::fprintf(stderr, \"mem=%lld,%lld\\n\", gll[0], gll[1]);\n"
                               "    else if (mode == 3)\n"
                               "      std::fprintf(stderr, \"mem=%d,%d\\n\", static_cast<int>(gsc[0]), static_cast<int>(gsc[1]));\n"
                               "    else\n"
                               "      std::fprintf(stderr, \"mem=%d,%d\\n\", gs[0], gs[1]);\n"
                               "    std::fflush(stderr);\n"
                               "    std::abort();\n"
                               "  }\n"
                               "}\n"
                               "int main(int argc, char** argv) {\n"
                               "  const char* arg = (argc > 1 && argv[1]) ? argv[1] : \"ovf\";\n"
                               "  if (std::strcmp(arg, \"uovf\") == 0) {\n"
                               "    mode = 1;\n"
                               "    volatile unsigned s = UINT_MAX;\n"
                               "    std::iota(gu, gu + 2, s);\n"
                               "    std::printf(\"wrapped=%u,%u\\n\", gu[0], gu[1]);\n"
                               "  } else if (std::strcmp(arg, \"ll\") == 0) {\n"
                               "    mode = 2;\n"
                               "    volatile long long s = LLONG_MAX;\n"
                               "    std::iota(gll, gll + 2, s);\n"
                               "    std::printf(\"wrapped=%lld,%lld\\n\", gll[0], gll[1]);\n"
                               "  } else if (std::strcmp(arg, \"sc\") == 0) {\n"
                               "    mode = 3;\n"
                               "    volatile signed char s = static_cast<signed char>(SCHAR_MAX);\n"
                               "    std::iota(gsc, gsc + 2, s);\n"
                               "    std::printf(\"wrapped=%d,%d\\n\", static_cast<int>(gsc[0]), static_cast<int>(gsc[1]));\n"
                               "  } else if (std::strcmp(arg, \"inv\") == 0) {\n"
                               "    mode = 0;\n"
                               "    std::iota(gs + 1, gs, 0);\n"
                               "    std::printf(\"wrapped=%d,%d\\n\", gs[0], gs[1]);\n"
                               "  } else if (std::strcmp(arg, \"max1\") == 0) {\n"
                               "    volatile int s = INT_MAX;\n"
                               "    std::iota(gs, gs + 1, s);\n"
                               "    std::printf(\"iota_hi=%d\\n\", gs[0]);\n"
                               "    return 0;\n"
                               "  } else if (std::strcmp(arg, \"empty\") == 0) {\n"
                               "    int e[1] = {7};\n"
                               "    std::iota(e, e, 1);\n"
                               "    std::printf(\"empty=%d\\n\", e[0]);\n"
                               "    return 0;\n"
                               "  } else if (std::strcmp(arg, \"scok\") == 0) {\n"
                               "    signed char a[2] = {};\n"
                               "    std::iota(a, a + 2, static_cast<signed char>(SCHAR_MAX - 1));\n"
                               "    std::printf(\"scok=%d,%d\\n\", static_cast<int>(a[0]), static_cast<int>(a[1]));\n"
                               "    return 0;\n"
                               "  } else {\n"
                               "    mode = 0;\n"
                               "    volatile int s = INT_MAX;\n"
                               "    std::iota(gs, gs + 2, s);\n"
                               "    std::printf(\"wrapped=%d,%d\\n\", gs[0], gs[1]);\n"
                               "  }\n"
                               "  return 0;\n"
                               "}\n";
#ifdef _WIN32
    const fs::path iota_mem_exe_out = c.tmp / "iota_mem.exe";
#else
    const fs::path iota_mem_exe_out = c.tmp / "iota_mem";
#endif
    std::vector<std::string> mem_build{c.cxx, "-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-g", "-O1", "-w", "-isystem",
                                       models.string(), iota_mem.string(), "-o", iota_mem_exe_out.string()};
    REQUIRE(run_cmd(mem_build));
    std::string mout, merr;
    CHECK_FALSE(run_cmd({iota_mem_exe_out.string(), "ovf"}, &mout, &merr));
    std::string mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::iota next value does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(INT_MAX) + ",5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({iota_mem_exe_out.string(), "uovf"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::iota next value does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(UINT_MAX) + ",5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({iota_mem_exe_out.string(), "ll"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::iota next value does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(LLONG_MAX) + ",5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({iota_mem_exe_out.string(), "sc"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::iota next value does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(SCHAR_MAX) + ",1") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({iota_mem_exe_out.string(), "inv"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::iota last is before first") != std::string::npos);
    CHECK(mboth.find("mem=4,5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    std::string mok;
    REQUIRE(run_cmd({iota_mem_exe_out.string(), "max1"}, &mok));
    CHECK(mok.find("iota_hi=" + std::to_string(INT_MAX)) != std::string::npos);
    REQUIRE(run_cmd({iota_mem_exe_out.string(), "empty"}, &mok));
    CHECK(mok.find("empty=7") != std::string::npos);
    REQUIRE(run_cmd({iota_mem_exe_out.string(), "scok"}, &mok));
    CHECK(mok.find("scok=" + std::to_string(SCHAR_MAX - 1) + "," + std::to_string(SCHAR_MAX)) != std::string::npos);

    const fs::path iota_const = c.tmp / "iota_const.cpp";
    std::ofstream(iota_const) << "#include <numeric>\n"
                                 "#include <climits>\n"
                                 "constexpr int hi() {\n"
                                 "  int a[1] = {0};\n"
                                 "  std::iota(a, a + 1, INT_MAX);\n"
                                 "  return a[0];\n"
                                 "}\n"
                                 "static_assert(hi() == INT_MAX);\n"
                                 "constexpr int near() {\n"
                                 "  int a[2] = {};\n"
                                 "  std::iota(a, a + 2, INT_MAX - 1);\n"
                                 "  return (a[0] == INT_MAX - 1 && a[1] == INT_MAX) ? 1 : 0;\n"
                                 "}\n"
                                 "static_assert(near() == 1);\n"
                                 "constexpr int empty() {\n"
                                 "  int a[1] = {7};\n"
                                 "  std::iota(a, a, 3);\n"
                                 "  return a[0];\n"
                                 "}\n"
                                 "static_assert(empty() == 7);\n";
    CHECK(syntax_only(c.cxx, iota_const, true));

    const fs::path iota_bad = c.tmp / "iota_bad.cpp";
    std::ofstream(iota_bad) << "#include <numeric>\n"
                               "#include <climits>\n"
                               "constexpr int bad() {\n"
                               "  int a[2] = {};\n"
                               "  std::iota(a, a + 2, INT_MAX);\n"
                               "  return a[1];\n"
                               "}\n"
                               "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, iota_bad, true));

    const fs::path iota_ubad = c.tmp / "iota_ubad.cpp";
    std::ofstream(iota_ubad) << "#include <numeric>\n"
                                "#include <climits>\n"
                                "constexpr unsigned bad() {\n"
                                "  unsigned a[2] = {};\n"
                                "  std::iota(a, a + 2, UINT_MAX);\n"
                                "  return a[1];\n"
                                "}\n"
                                "constexpr unsigned v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, iota_ubad, true));

    const fs::path iota_inv = c.tmp / "iota_inv.cpp";
    std::ofstream(iota_inv) << "#include <numeric>\n"
                               "constexpr int bad() {\n"
                               "  int a[2] = {4, 5};\n"
                               "  std::iota(a + 1, a, 0);\n"
                               "  return a[0];\n"
                               "}\n"
                               "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, iota_inv, true));

    std::string aout;
    REQUIRE(run_cmd({mod.string(), "acc-max1"}, &aout));
    CHECK(aout.find("acc_hi=" + std::to_string(INT_MAX)) != std::string::npos);

    auto expect_acc_trap = [&](const char* arg, const char* cls, const char* msg) {
        std::string out, err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &out, &err));
        const std::string both = out + err;
        CHECK(both.find(std::string("CHECK ") + cls + ":") != std::string::npos);
        CHECK(both.find(msg) != std::string::npos);
        CHECK(out.find("wrapped=") == std::string::npos);
    };
    expect_acc_trap("acc-ovf", "INT-SIGNED-OVF", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-llovf", "INT-SIGNED-OVF", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-minovf", "INT-SIGNED-OVF", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-sc", "INT-SIGNED-OVF", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-part", "INT-SIGNED-OVF", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-uovf", "FUNC-CONTRACT", "std::accumulate sum does not fit in T");
    expect_acc_trap("acc-inv", "FUNC-CONTRACT", "std::accumulate last is before first");

    const fs::path acc_const = c.tmp / "acc_const.cpp";
    std::ofstream(acc_const) << "#include <numeric>\n"
                                "#include <climits>\n"
                                "constexpr int seq() {\n"
                                "  int a[4] = {1, 2, 3, 4};\n"
                                "  return std::accumulate(a, a + 4, 0);\n"
                                "}\n"
                                "static_assert(seq() == 10);\n"
                                "constexpr int empty() {\n"
                                "  int a[1] = {7};\n"
                                "  return std::accumulate(a, a, 3);\n"
                                "}\n"
                                "static_assert(empty() == 3);\n"
                                "constexpr int hi() {\n"
                                "  int a[1] = {0};\n"
                                "  return std::accumulate(a, a + 1, INT_MAX);\n"
                                "}\n"
                                "static_assert(hi() == INT_MAX);\n"
                                "constexpr int near() {\n"
                                "  int a[2] = {1, 1};\n"
                                "  return std::accumulate(a, a + 2, INT_MAX - 2);\n"
                                "}\n"
                                "static_assert(near() == INT_MAX);\n"
                                "constexpr int imin() {\n"
                                "  int a[1] = {INT_MIN};\n"
                                "  return std::accumulate(a, a + 1, 0);\n"
                                "}\n"
                                "static_assert(imin() == INT_MIN);\n";
    CHECK(syntax_only(c.cxx, acc_const, true));
    CHECK(syntax_only(c.cxx, acc_const, false));

    const fs::path acc_bad = c.tmp / "acc_bad.cpp";
    std::ofstream(acc_bad) << "#include <numeric>\n"
                              "#include <climits>\n"
                              "constexpr int bad() {\n"
                              "  int a[1] = {1};\n"
                              "  return std::accumulate(a, a + 1, INT_MAX);\n"
                              "}\n"
                              "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, acc_bad, true));

    const fs::path acc_ubad = c.tmp / "acc_ubad.cpp";
    std::ofstream(acc_ubad) << "#include <numeric>\n"
                               "#include <climits>\n"
                               "constexpr unsigned bad() {\n"
                               "  unsigned a[1] = {1u};\n"
                               "  return std::accumulate(a, a + 1, UINT_MAX);\n"
                               "}\n"
                               "constexpr unsigned v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, acc_ubad, true));

    const fs::path acc_inv = c.tmp / "acc_inv.cpp";
    std::ofstream(acc_inv) << "#include <numeric>\n"
                              "constexpr int bad() {\n"
                              "  int a[2] = {4, 5};\n"
                              "  return std::accumulate(a + 1, a, 0);\n"
                              "}\n"
                              "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, acc_inv, true));
}
