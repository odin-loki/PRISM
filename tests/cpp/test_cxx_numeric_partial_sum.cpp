// std::partial_sum on the partial <numeric> model
// (src/prism/pir/models/cxx/numeric). CMake's prism_tests source list is
// explicit (not a glob): this file is one line in _prism_test_sources.
// The trace is tests/cxx_models/numeric_trace.cpp.
//
// last < first, a null destination, and a destination in [first, last) are
// checks, then a trap, and those paths do not write. An empty range returns
// dest and does not read or write. A sum that does not fit is a check, then
// a trap, and that path does not store a wrapped sum. This is not a proof.

#include <doctest/doctest.h>

#include <climits>
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
                            "int main() {\n"
                            "  int a[3] = {1, 2, 3};\n"
                            "  int d[3] = {};\n"
                            "  int* end = std::partial_sum(a, a + 3, d);\n"
                            "  if (end != d + 3 || d[0] != 1 || d[1] != 3 || d[2] != 6) return 1;\n"
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

TEST_CASE("cxx numeric partial_sum: traps before a bad range, a null dest, or a sum that does not fit") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_NUMERIC") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("#include <iterator>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("#include <algorithm>") == std::string::npos);
    CHECK(text.find("a + b") == std::string::npos);
    CHECK(text.find("a+b") == std::string::npos);
    CHECK(text.find("(a+b)/2") == std::string::npos);
    CHECK(text.find("\nreduce(") == std::string::npos);
    CHECK(text.find("\ntransform_reduce(") == std::string::npos);
    CHECK(text.find("\ninner_product(") == std::string::npos);
    CHECK(text.find("\nexclusive_scan(") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(text.find("\nmidpoint(") != std::string::npos);
    CHECK(text.find("\ngcd(") != std::string::npos);
    CHECK(text.find("\nlcm(") != std::string::npos);
    CHECK(text.find("\niota(") != std::string::npos);
    CHECK(text.find("\naccumulate(") != std::string::npos);
    CHECK(text.find("\nadjacent_difference(") != std::string::npos);
    CHECK(text.find("while (__un != 0)") != std::string::npos);
    const auto ps_at = text.find("\npartial_sum(");
    REQUIRE(ps_at != std::string::npos);
    const std::string ps = text.substr(ps_at);
    const auto inv = ps.find("if (__last < __first)");
    const auto inv_check = ps.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::partial_sum last is before first\")");
    const auto empty = ps.find("if (__first == __last) return __dest;");
    const auto null_if = ps.find("if (__dest == nullptr)");
    const auto null_check = ps.find("__prism_check(0, \"PTR-NULL-DEREF\", \"std::partial_sum null destination\")");
    const auto over_check =
        ps.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::partial_sum dest lies in [first, last)\")");
    const auto read = ps.find("*__first");
    const auto copy = ps.find("*__dest = __sum;");
    const auto ovf = ps.find("__prism_check(0, \"INT-SIGNED-OVF\", \"std::partial_sum sum does not fit in T\")");
    const auto uovf = ps.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::partial_sum sum does not fit in T\")");
    const char* kAdd = "__sum = static_cast<_Tp>(__sum + __elem);";
    const auto step = ps.find(kAdd);
    const auto step2 = step == std::string::npos ? std::string::npos : ps.find(kAdd, step + 1);
    const auto step3 = step2 == std::string::npos ? std::string::npos : ps.find(kAdd, step2 + 1);
    const auto step4 = step3 == std::string::npos ? std::string::npos : ps.find(kAdd, step3 + 1);
    REQUIRE(inv != std::string::npos);
    REQUIRE(inv_check != std::string::npos);
    REQUIRE(empty != std::string::npos);
    REQUIRE(null_if != std::string::npos);
    REQUIRE(null_check != std::string::npos);
    REQUIRE(over_check != std::string::npos);
    REQUIRE(read != std::string::npos);
    REQUIRE(copy != std::string::npos);
    REQUIRE(ovf != std::string::npos);
    REQUIRE(uovf != std::string::npos);
    REQUIRE(step != std::string::npos);
    REQUIRE(step2 != std::string::npos);
    REQUIRE(step3 != std::string::npos);
    CHECK(step4 == std::string::npos);
    CHECK(inv < inv_check);
    CHECK(inv_check < empty);
    CHECK(empty < null_if);
    CHECK(null_if < null_check);
    CHECK(null_check < over_check);
    CHECK(over_check < read);
    CHECK(read < copy);
    CHECK(copy < ovf);
    const auto inv_trap = ps.find("__builtin_trap()", inv_check);
    REQUIRE(inv_trap != std::string::npos);
    CHECK(inv_trap < empty);
    const std::string inv_arm = ps.substr(inv_check, inv_trap - inv_check);
    CHECK(inv_arm.find("*__first") == std::string::npos);
    CHECK(inv_arm.find("*__dest") == std::string::npos);
    CHECK(inv_arm.find("__sum +") == std::string::npos);
    CHECK(inv_arm.find("return") == std::string::npos);
    const auto null_trap = ps.find("__builtin_trap()", null_check);
    REQUIRE(null_trap != std::string::npos);
    CHECK(null_trap < over_check);
    const std::string null_arm = ps.substr(null_check, null_trap - null_check);
    CHECK(null_arm.find("*__first") == std::string::npos);
    CHECK(null_arm.find("*__dest") == std::string::npos);
    CHECK(null_arm.find("__sum +") == std::string::npos);
    CHECK(null_arm.find("return") == std::string::npos);
    const auto over_trap = ps.find("__builtin_trap()", over_check);
    REQUIRE(over_trap != std::string::npos);
    CHECK(over_trap < read);
    const std::string over_arm = ps.substr(over_check, over_trap - over_check);
    CHECK(over_arm.find("*__first") == std::string::npos);
    CHECK(over_arm.find("*__dest") == std::string::npos);
    CHECK(over_arm.find("__sum +") == std::string::npos);
    CHECK(over_arm.find("return") == std::string::npos);
    const auto ovf_trap = ps.find("__builtin_trap()", ovf);
    REQUIRE(ovf_trap != std::string::npos);
    CHECK(ovf_trap < step);
    CHECK(step < uovf);
    const auto ovf_else = ps.find("else", ovf_trap);
    REQUIRE(ovf_else != std::string::npos);
    CHECK(ovf_else < step);
    const std::string ovf_arm = ps.substr(ovf, ovf_trap - ovf);
    CHECK(ovf_arm.find("__sum +") == std::string::npos);
    CHECK(ovf_arm.find("*__dest") == std::string::npos);
    CHECK(ovf_arm.find("return") == std::string::npos);
    const auto u_trap = ps.find("__builtin_trap()", uovf);
    REQUIRE(u_trap != std::string::npos);
    CHECK(u_trap < step2);
    const auto u_else = ps.find("else", u_trap);
    REQUIRE(u_else != std::string::npos);
    CHECK(u_else < step2);
    const std::string u_arm = ps.substr(uovf, u_trap - uovf);
    CHECK(u_arm.find("__sum +") == std::string::npos);
    CHECK(u_arm.find("*__dest") == std::string::npos);
    CHECK(u_arm.find("return") == std::string::npos);
    CHECK(step2 < step3);
    const std::string third = ps.substr(step2, step3 - step2);
    CHECK(third.find("__prism_check") == std::string::npos);
    const auto mid = text.find("midpoint(");
    const auto gcd_at = text.find("\ngcd(", mid);
    REQUIRE(mid != std::string::npos);
    REQUIRE(gcd_at != std::string::npos);
    const std::string mid_body = text.substr(mid, gcd_at - mid);
    CHECK(mid_body.find("a + b") == std::string::npos);
    CHECK(mid_body.find("a+b") == std::string::npos);
    CHECK(mid_body.find("(a+b)/2") == std::string::npos);
}

TEST_CASE("cxx numeric partial_sum: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxnumeric-partial-sum";
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

TEST_CASE("cxx numeric partial_sum: prefix sums match and a sum that does not fit does not store") {
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
    CHECK(has_line(b, "ps=1,3,6,10 end=4"));
    CHECK(has_line(b, "ps_empty=9,7 end=1"));
    CHECK(has_line(b, "ps_neg=-5,-2,2"));
    CHECK(has_line(b, "ps_imin=0," + std::to_string(INT_MIN) + "," + std::to_string(INT_MIN + 1)));
    CHECK(has_line(b, "ps_near=" + std::to_string(INT_MAX - 2) + "," + std::to_string(INT_MAX - 1) + "," +
                          std::to_string(INT_MAX)));
    CHECK(has_line(b, "ps_down=" + std::to_string(INT_MAX) + "," + std::to_string(INT_MAX - 5) + "," +
                          std::to_string(INT_MAX - 2)));
    CHECK(has_line(b, "ps_zeroadd=" + std::to_string(INT_MAX) + "," + std::to_string(INT_MAX)));
    CHECK(has_line(b, "ps_min0=" + std::to_string(INT_MIN) + "," + std::to_string(INT_MIN)));
    CHECK(has_line(b, "ps_eq=4,8,12"));
    CHECK(has_line(b, "ps_u=1,3,6"));
    CHECK(has_line(b, "ps_unear=" + std::to_string(UINT_MAX - 2u) + "," + std::to_string(UINT_MAX - 1u) + "," +
                          std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ps_umax=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ps_uzero=" + std::to_string(UINT_MAX) + "," + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ps_one=41 end=1"));
    CHECK(has_line(b, "ps_hi=" + std::to_string(INT_MAX) + " end=1"));
    CHECK(has_line(b, "ps_sc=" + std::to_string(SCHAR_MAX - 2) + "," + std::to_string(SCHAR_MAX - 1) + "," +
                          std::to_string(SCHAR_MAX)));
    CHECK(has_line(b, "ps_scn=-2,-5"));
    CHECK(has_line(b, "ps_uc=" + std::to_string(UCHAR_MAX - 5) + "," + std::to_string(UCHAR_MAX - 3) + "," +
                          std::to_string(UCHAR_MAX)));
    CHECK(has_line(b, "ps_f=2,5,9"));
    CHECK(has_line(b, "ps_b=0,1,1"));
    CHECK(has_line(b, "ps_bf=0,0,1"));
    CHECK(has_line(b, "ps_tail=1,2,1,3 end=2"));
    CHECK(has_line(b, "ps_empty_null=1"));
    CHECK(has_line(b, "ps_ll=1,3,6"));
    CHECK(has_line(b, "gcd00=0"));

    std::string hi_out;
    REQUIRE(run_cmd({mod.string(), "ps-max1"}, &hi_out));
    CHECK(hi_out.find("ps_hi=" + std::to_string(INT_MAX) + " end=1") != std::string::npos);

    auto expect_trap = [&](const char* arg, const char* cls, const char* msg) {
        std::string out, err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &out, &err));
        const std::string both = out + err;
        CHECK(both.find(std::string("CHECK ") + cls + ":") != std::string::npos);
        CHECK(both.find(msg) != std::string::npos);
        CHECK(out.find("wrapped=") == std::string::npos);
    };
    expect_trap("ps-ovf", "INT-SIGNED-OVF", "std::partial_sum sum does not fit in T");
    expect_trap("ps-llovf", "INT-SIGNED-OVF", "std::partial_sum sum does not fit in T");
    expect_trap("ps-minovf", "INT-SIGNED-OVF", "std::partial_sum sum does not fit in T");
    expect_trap("ps-sc", "INT-SIGNED-OVF", "std::partial_sum sum does not fit in T");
    expect_trap("ps-part", "INT-SIGNED-OVF", "std::partial_sum sum does not fit in T");
    expect_trap("ps-uovf", "FUNC-CONTRACT", "std::partial_sum sum does not fit in T");
    expect_trap("ps-ucovf", "FUNC-CONTRACT", "std::partial_sum sum does not fit in T");
    expect_trap("ps-inv", "FUNC-CONTRACT", "std::partial_sum last is before first");
    expect_trap("ps-null", "PTR-NULL-DEREF", "std::partial_sum null destination");
    expect_trap("ps-over", "FUNC-CONTRACT", "std::partial_sum dest lies in [first, last)");

    const fs::path ps_const = c.tmp / "ps_const.cpp";
    std::ofstream(ps_const) << "#include <numeric>\n"
                               "#include <climits>\n"
                               "constexpr int seq() {\n"
                               "  int a[4] = {1, 2, 3, 4};\n"
                               "  int d[4] = {};\n"
                               "  int* end = std::partial_sum(a, a + 4, d);\n"
                               "  return (end == d + 4 && d[0] == 1 && d[1] == 3 && d[2] == 6 && d[3] == 10) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(seq() == 1);\n"
                               "constexpr int empty() {\n"
                               "  int a[1] = {7};\n"
                               "  int d[1] = {9};\n"
                               "  int* end = std::partial_sum(a, a, d);\n"
                               "  return (end == d && d[0] == 9 && a[0] == 7) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(empty() == 1);\n"
                               "constexpr int hi() {\n"
                               "  int a[1] = {INT_MAX};\n"
                               "  int d[1] = {0};\n"
                               "  int* end = std::partial_sum(a, a + 1, d);\n"
                               "  return (end == d + 1 && d[0] == INT_MAX) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(hi() == 1);\n"
                               "constexpr int imin() {\n"
                               "  int a[3] = {0, INT_MIN, 1};\n"
                               "  int d[3] = {};\n"
                               "  std::partial_sum(a, a + 3, d);\n"
                               "  return (d[0] == 0 && d[1] == INT_MIN && d[2] == INT_MIN + 1) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(imin() == 1);\n"
                               "constexpr int bits() {\n"
                               "  bool a[3] = {false, false, true};\n"
                               "  bool d[3] = {true, true, true};\n"
                               "  std::partial_sum(a, a + 3, d);\n"
                               "  return (d[0] == false && d[1] == false && d[2] == true) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(bits() == 1);\n";
    CHECK(syntax_only(c.cxx, ps_const, true));
    CHECK(syntax_only(c.cxx, ps_const, false));

    const fs::path ps_bad = c.tmp / "ps_bad.cpp";
    std::ofstream(ps_bad) << "#include <numeric>\n"
                             "#include <climits>\n"
                             "constexpr int bad() {\n"
                             "  int a[2] = {INT_MAX, 1};\n"
                             "  int d[2] = {};\n"
                             "  std::partial_sum(a, a + 2, d);\n"
                             "  return d[1];\n"
                             "}\n"
                             "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_bad, true));

    const fs::path ps_ubad = c.tmp / "ps_ubad.cpp";
    std::ofstream(ps_ubad) << "#include <numeric>\n"
                              "#include <climits>\n"
                              "constexpr unsigned bad() {\n"
                              "  unsigned a[2] = {1u, UINT_MAX};\n"
                              "  unsigned d[2] = {};\n"
                              "  std::partial_sum(a, a + 2, d);\n"
                              "  return d[1];\n"
                              "}\n"
                              "constexpr unsigned v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_ubad, true));

    const fs::path ps_inv = c.tmp / "ps_inv.cpp";
    std::ofstream(ps_inv) << "#include <numeric>\n"
                             "constexpr int bad() {\n"
                             "  int a[2] = {4, 5};\n"
                             "  int d[2] = {7, 8};\n"
                             "  std::partial_sum(a + 1, a, d);\n"
                             "  return d[0];\n"
                             "}\n"
                             "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_inv, true));

    const fs::path ps_null = c.tmp / "ps_null.cpp";
    std::ofstream(ps_null) << "#include <numeric>\n"
                              "constexpr int bad() {\n"
                              "  int a[1] = {3};\n"
                              "  int* d = nullptr;\n"
                              "  std::partial_sum(a, a + 1, d);\n"
                              "  return 0;\n"
                              "}\n"
                              "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_null, true));

    const fs::path ps_over = c.tmp / "ps_over.cpp";
    std::ofstream(ps_over) << "#include <numeric>\n"
                              "constexpr int bad() {\n"
                              "  int a[3] = {3, 5, 9};\n"
                              "  std::partial_sum(a, a + 3, a + 1);\n"
                              "  return a[1];\n"
                              "}\n"
                              "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_over, true));

    const fs::path ps_mix = c.tmp / "ps_mix.cpp";
    std::ofstream(ps_mix) << "#include <numeric>\n"
                             "int main() {\n"
                             "  int a[2] = {1, 2};\n"
                             "  long d[2] = {};\n"
                             "  std::partial_sum(a, a + 2, d);\n"
                             "  return 0;\n"
                             "}\n";
    CHECK_FALSE(syntax_only(c.cxx, ps_mix, true));
    CHECK(syntax_only(c.cxx, ps_mix, false));

    const fs::path ps_mem = c.tmp / "ps_mem.cpp";
    std::ofstream(ps_mem) << "#include <climits>\n"
                             "#include <cstdio>\n"
                             "#include <cstdlib>\n"
                             "#include <cstring>\n"
                             "#include <numeric>\n"
                             "static int gd[3] = {4, 5, 6};\n"
                             "static unsigned gu[2] = {4u, 5u};\n"
                             "static signed char gsc[2] = {4, 5};\n"
                             "static unsigned char guc[2] = {4, 5};\n"
                             "static int gs[3] = {3, 5, 9};\n"
                             "static int mode = 0;\n"
                             "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
                             "  if (!ok) {\n"
                             "    std::fprintf(stderr, \"CHECK %s: %s\\n\", cls ? cls : \"\", msg ? msg : \"\");\n"
                             "    if (mode == 1)\n"
                             "      std::fprintf(stderr, \"mem=%u,%u\\n\", gu[0], gu[1]);\n"
                             "    else if (mode == 3)\n"
                             "      std::fprintf(stderr, \"mem=%d,%d\\n\", static_cast<int>(gsc[0]), static_cast<int>(gsc[1]));\n"
                             "    else if (mode == 4)\n"
                             "      std::fprintf(stderr, \"mem=%d,%d,%d\\n\", gs[0], gs[1], gs[2]);\n"
                             "    else if (mode == 5)\n"
                             "      std::fprintf(stderr, \"mem=%u,%u\\n\", static_cast<unsigned>(guc[0]), static_cast<unsigned>(guc[1]));\n"
                             "    else\n"
                             "      std::fprintf(stderr, \"mem=%d,%d,%d\\n\", gd[0], gd[1], gd[2]);\n"
                             "    std::fflush(stderr);\n"
                             "    std::abort();\n"
                             "  }\n"
                             "}\n"
                             "int main(int argc, char** argv) {\n"
                             "  const char* arg = (argc > 1 && argv[1]) ? argv[1] : \"ovf\";\n"
                             "  if (std::strcmp(arg, \"uovf\") == 0) {\n"
                             "    mode = 1;\n"
                             "    unsigned s[2] = {1u, UINT_MAX};\n"
                             "    std::partial_sum(s, s + 2, gu);\n"
                             "    std::printf(\"wrapped=%u,%u\\n\", gu[0], gu[1]);\n"
                             "  } else if (std::strcmp(arg, \"sc\") == 0) {\n"
                             "    mode = 3;\n"
                             "    signed char s[2] = {static_cast<signed char>(SCHAR_MAX), 1};\n"
                             "    std::partial_sum(s, s + 2, gsc);\n"
                             "    std::printf(\"wrapped=%d,%d\\n\", static_cast<int>(gsc[0]), static_cast<int>(gsc[1]));\n"
                             "  } else if (std::strcmp(arg, \"uc\") == 0) {\n"
                             "    mode = 5;\n"
                             "    unsigned char s[2] = {static_cast<unsigned char>(UCHAR_MAX), 1};\n"
                             "    std::partial_sum(s, s + 2, guc);\n"
                             "    std::printf(\"wrapped=%u,%u\\n\", static_cast<unsigned>(guc[0]), static_cast<unsigned>(guc[1]));\n"
                             "  } else if (std::strcmp(arg, \"part\") == 0) {\n"
                             "    mode = 0;\n"
                             "    int s[3] = {5, 8, INT_MAX};\n"
                             "    std::partial_sum(s, s + 3, gd);\n"
                             "    std::printf(\"wrapped=%d,%d,%d\\n\", gd[0], gd[1], gd[2]);\n"
                             "  } else if (std::strcmp(arg, \"min\") == 0) {\n"
                             "    mode = 0;\n"
                             "    int s[2] = {INT_MIN, -1};\n"
                             "    std::partial_sum(s, s + 2, gd);\n"
                             "    std::printf(\"wrapped=%d,%d\\n\", gd[0], gd[1]);\n"
                             "  } else if (std::strcmp(arg, \"inv\") == 0) {\n"
                             "    mode = 0;\n"
                             "    int s[2] = {4, 5};\n"
                             "    std::partial_sum(s + 1, s, gd);\n"
                             "    std::printf(\"wrapped=%d,%d\\n\", gd[0], gd[1]);\n"
                             "  } else if (std::strcmp(arg, \"over\") == 0) {\n"
                             "    mode = 4;\n"
                             "    std::partial_sum(gs, gs + 3, gs + 1);\n"
                             "    std::printf(\"wrapped=%d,%d,%d\\n\", gs[0], gs[1], gs[2]);\n"
                             "  } else if (std::strcmp(arg, \"null\") == 0) {\n"
                             "    mode = 0;\n"
                             "    int s[1] = {3};\n"
                             "    std::partial_sum(s, s + 1, static_cast<int*>(nullptr));\n"
                             "    std::printf(\"wrapped=1\\n\");\n"
                             "  } else if (std::strcmp(arg, \"max1\") == 0) {\n"
                             "    int d[1] = {0};\n"
                             "    int s[1] = {INT_MAX};\n"
                             "    int* end = std::partial_sum(s, s + 1, d);\n"
                             "    std::printf(\"ps_hi=%d end=%d\\n\", d[0], static_cast<int>(end - d));\n"
                             "    return 0;\n"
                             "  } else if (std::strcmp(arg, \"empty\") == 0) {\n"
                             "    int s[1] = {4};\n"
                             "    int d[1] = {7};\n"
                             "    int* end = std::partial_sum(s, s, d);\n"
                             "    std::printf(\"empty=%d end=%d\\n\", d[0], end == d ? 1 : 0);\n"
                             "    return 0;\n"
                             "  } else {\n"
                             "    mode = 0;\n"
                             "    int s[2] = {INT_MAX, 1};\n"
                             "    std::partial_sum(s, s + 2, gd);\n"
                             "    std::printf(\"wrapped=%d,%d,%d\\n\", gd[0], gd[1], gd[2]);\n"
                             "  }\n"
                             "  return 0;\n"
                             "}\n";
#ifdef _WIN32
    const fs::path ps_mem_exe = c.tmp / "ps_mem.exe";
#else
    const fs::path ps_mem_exe = c.tmp / "ps_mem";
#endif
    std::vector<std::string> mem_build{c.cxx, "-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-g", "-O1", "-w", "-isystem",
                                       models.string(), ps_mem.string(), "-o", ps_mem_exe.string()};
    REQUIRE(run_cmd(mem_build));
    std::string mout, merr;
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "ovf"}, &mout, &merr));
    std::string mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(INT_MAX) + ",5,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "min"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(INT_MIN) + ",5,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "uovf"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=1,5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "sc"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(SCHAR_MAX) + ",5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "uc"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(UCHAR_MAX) + ",5") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "part"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::partial_sum sum does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=5,13,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "inv"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::partial_sum last is before first") != std::string::npos);
    CHECK(mboth.find("mem=4,5,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "over"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::partial_sum dest lies in [first, last)") != std::string::npos);
    CHECK(mboth.find("mem=3,5,9") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ps_mem_exe.string(), "null"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK PTR-NULL-DEREF: std::partial_sum null destination") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    std::string mok;
    REQUIRE(run_cmd({ps_mem_exe.string(), "max1"}, &mok));
    CHECK(mok.find("ps_hi=" + std::to_string(INT_MAX) + " end=1") != std::string::npos);
    REQUIRE(run_cmd({ps_mem_exe.string(), "empty"}, &mok));
    CHECK(mok.find("empty=7 end=1") != std::string::npos);
}
