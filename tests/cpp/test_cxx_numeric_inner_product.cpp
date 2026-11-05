// std::inner_product on the partial <numeric> model
// (src/prism/pir/models/cxx/numeric). CMake's prism_tests source list is
// explicit (not a glob): this file is one line in _prism_test_sources.
// The trace is tests/cxx_models/numeric_trace.cpp.
//
// last < first and a null second range are checks, then a trap, and those
// paths do not read. An empty range returns init and does not read, even
// when the second pointer is null. A product or a sum that does not fit is
// a check, then a trap, and that path does not store a wrapped value. This
// is not a proof.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

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

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <numeric>\n"
                                  "int main() {\n"
                                  "  int a[3] = {1, 2, 3};\n"
                                  "  int b[3] = {4, 5, 6};\n"
                                  "  if (std::inner_product(a, a + 3, b, 0) != 32) return 1;\n"
                                  "  if (std::inner_product(a, a, static_cast<int*>(nullptr), 9) != 9) return 1;\n"
                                  "  return 0;\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("numeric_trace.cpp"), "numeric_trace", cxxh::asan_flags(), with_models);
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

TEST_CASE("cxx numeric inner_product: traps before a bad range, a null second range, or a result that does not fit") {
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
    CHECK(text.find("__a + __b") == std::string::npos);
    CHECK(text.find("(a+b)/2") == std::string::npos);
    CHECK(text.find("\nreduce(") == std::string::npos);
    CHECK(text.find("\ntransform_reduce(") == std::string::npos);
    CHECK(text.find("\nexclusive_scan(") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(text.find("\nmidpoint(") != std::string::npos);
    CHECK(text.find("\ngcd(") != std::string::npos);
    CHECK(text.find("\nlcm(") != std::string::npos);
    CHECK(text.find("\niota(") != std::string::npos);
    CHECK(text.find("\naccumulate(") != std::string::npos);
    CHECK(text.find("\nadjacent_difference(") != std::string::npos);
    CHECK(text.find("\npartial_sum(") != std::string::npos);
    CHECK(text.find("while (__un != 0)") != std::string::npos);
    const auto ip_at = text.find("\ninner_product(");
    REQUIRE(ip_at != std::string::npos);
    const std::string ip = text.substr(ip_at);
    const auto inv = ip.find("if (__last1 < __first1)");
    const auto inv_check =
        ip.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::inner_product last is before first\")");
    const auto empty = ip.find("if (__first1 == __last1) return __init;");
    const auto null_if = ip.find("if (__first2 == nullptr)");
    const auto null_check =
        ip.find("__prism_check(0, \"PTR-NULL-DEREF\", \"std::inner_product null second range\")");
    const auto read1 = ip.find("*__first1");
    const auto read2 = ip.find("*__first2");
    const char* kOvf = "__prism_check(0, \"INT-SIGNED-OVF\", \"std::inner_product result does not fit in T\")";
    const char* kUovf = "__prism_check(0, \"FUNC-CONTRACT\", \"std::inner_product result does not fit in T\")";
    const char* kMul = "const _Tp __prod = static_cast<_Tp>(__a * __b);";
    const char* kAdd = "__init = static_cast<_Tp>(__init + __prod);";
    const char* kPlain = "__init = static_cast<_Tp>(__init + (__a * __b));";
    const auto ovf = ip.find(kOvf);
    const auto ovf2 = ovf == std::string::npos ? std::string::npos : ip.find(kOvf, ovf + 1);
    const auto ovf3 = ovf2 == std::string::npos ? std::string::npos : ip.find(kOvf, ovf2 + 1);
    const auto uovf = ip.find(kUovf);
    const auto uovf2 = uovf == std::string::npos ? std::string::npos : ip.find(kUovf, uovf + 1);
    const auto uovf3 = uovf2 == std::string::npos ? std::string::npos : ip.find(kUovf, uovf2 + 1);
    const auto mul = ip.find(kMul);
    const auto mul2 = mul == std::string::npos ? std::string::npos : ip.find(kMul, mul + 1);
    const auto mul3 = mul2 == std::string::npos ? std::string::npos : ip.find(kMul, mul2 + 1);
    const auto add = ip.find(kAdd);
    const auto add2 = add == std::string::npos ? std::string::npos : ip.find(kAdd, add + 1);
    const auto add3 = add2 == std::string::npos ? std::string::npos : ip.find(kAdd, add2 + 1);
    const auto plain = ip.find(kPlain);
    const auto plain2 = plain == std::string::npos ? std::string::npos : ip.find(kPlain, plain + 1);
    const auto advance = ip.find("++__first1");
    REQUIRE(inv != std::string::npos);
    REQUIRE(inv_check != std::string::npos);
    REQUIRE(empty != std::string::npos);
    REQUIRE(null_if != std::string::npos);
    REQUIRE(null_check != std::string::npos);
    REQUIRE(read1 != std::string::npos);
    REQUIRE(read2 != std::string::npos);
    REQUIRE(ovf != std::string::npos);
    REQUIRE(ovf2 != std::string::npos);
    CHECK(ovf3 == std::string::npos);
    REQUIRE(uovf != std::string::npos);
    REQUIRE(uovf2 != std::string::npos);
    CHECK(uovf3 == std::string::npos);
    REQUIRE(mul != std::string::npos);
    REQUIRE(mul2 != std::string::npos);
    CHECK(mul3 == std::string::npos);
    REQUIRE(add != std::string::npos);
    REQUIRE(add2 != std::string::npos);
    CHECK(add3 == std::string::npos);
    REQUIRE(plain != std::string::npos);
    CHECK(plain2 == std::string::npos);
    REQUIRE(advance != std::string::npos);
    CHECK(inv < inv_check);
    CHECK(inv_check < empty);
    CHECK(empty < null_if);
    CHECK(null_if < null_check);
    CHECK(null_check < read1);
    CHECK(read1 < read2);
    CHECK(read2 < ovf);
    const auto inv_trap = ip.find("__builtin_trap()", inv_check);
    REQUIRE(inv_trap != std::string::npos);
    CHECK(inv_trap < empty);
    const std::string inv_arm = ip.substr(inv_check, inv_trap - inv_check);
    CHECK(inv_arm.find("*") == std::string::npos);
    CHECK(inv_arm.find("__a *") == std::string::npos);
    CHECK(inv_arm.find("__init +") == std::string::npos);
    CHECK(inv_arm.find("return") == std::string::npos);
    const auto null_trap = ip.find("__builtin_trap()", null_check);
    REQUIRE(null_trap != std::string::npos);
    CHECK(null_trap < read1);
    const std::string null_arm = ip.substr(null_check, null_trap - null_check);
    CHECK(null_arm.find("*") == std::string::npos);
    CHECK(null_arm.find("__a *") == std::string::npos);
    CHECK(null_arm.find("__init +") == std::string::npos);
    CHECK(null_arm.find("return") == std::string::npos);
    const auto ovf_trap = ip.find("__builtin_trap()", ovf);
    REQUIRE(ovf_trap != std::string::npos);
    CHECK(ovf_trap < mul);
    const auto ovf_else = ip.find("else", ovf_trap);
    REQUIRE(ovf_else != std::string::npos);
    CHECK(ovf_else < mul);
    const std::string ovf_arm = ip.substr(ovf, ovf_trap - ovf);
    CHECK(ovf_arm.find("__a *") == std::string::npos);
    CHECK(ovf_arm.find("__init +") == std::string::npos);
    CHECK(ovf_arm.find("return") == std::string::npos);
    const auto ovf2_trap = ip.find("__builtin_trap()", ovf2);
    REQUIRE(ovf2_trap != std::string::npos);
    CHECK(mul < ovf2);
    CHECK(ovf2_trap < add);
    const auto ovf2_else = ip.find("else", ovf2_trap);
    REQUIRE(ovf2_else != std::string::npos);
    CHECK(ovf2_else < add);
    const std::string ovf2_arm = ip.substr(ovf2, ovf2_trap - ovf2);
    CHECK(ovf2_arm.find("__init +") == std::string::npos);
    CHECK(ovf2_arm.find("return") == std::string::npos);
    const auto u_trap = ip.find("__builtin_trap()", uovf);
    REQUIRE(u_trap != std::string::npos);
    CHECK(add < uovf);
    CHECK(u_trap < mul2);
    const auto u_else = ip.find("else", u_trap);
    REQUIRE(u_else != std::string::npos);
    CHECK(u_else < mul2);
    const std::string u_arm = ip.substr(uovf, u_trap - uovf);
    CHECK(u_arm.find("__a *") == std::string::npos);
    CHECK(u_arm.find("__init +") == std::string::npos);
    CHECK(u_arm.find("return") == std::string::npos);
    const auto u2_trap = ip.find("__builtin_trap()", uovf2);
    REQUIRE(u2_trap != std::string::npos);
    CHECK(mul2 < uovf2);
    CHECK(u2_trap < add2);
    const auto u2_else = ip.find("else", u2_trap);
    REQUIRE(u2_else != std::string::npos);
    CHECK(u2_else < add2);
    const std::string u2_arm = ip.substr(uovf2, u2_trap - uovf2);
    CHECK(u2_arm.find("__init +") == std::string::npos);
    CHECK(u2_arm.find("return") == std::string::npos);
    CHECK(add2 < plain);
    const std::string plain_gap = ip.substr(add2, plain - add2);
    CHECK(plain_gap.find("__prism_check") == std::string::npos);
    CHECK(plain < advance);
    const auto mid = text.find("midpoint(");
    const auto gcd_at = text.find("\ngcd(", mid);
    REQUIRE(mid != std::string::npos);
    REQUIRE(gcd_at != std::string::npos);
    const std::string mid_body = text.substr(mid, gcd_at - mid);
    CHECK(mid_body.find("a + b") == std::string::npos);
    CHECK(mid_body.find("a+b") == std::string::npos);
    CHECK(mid_body.find("(a+b)/2") == std::string::npos);
}

TEST_CASE("cxx numeric inner_product: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxnumeric-inner-product";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx numeric inner_product: products match and a result that does not fit is not stored") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
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
    CHECK(has_line(b, "ip=10"));
    CHECK(has_line(b, "ip_dot=14"));
    CHECK(has_line(b, "ip_init=33"));
    CHECK(has_line(b, "ip_empty=3,7"));
    CHECK(has_line(b, "ip_empty_null=3"));
    CHECK(has_line(b, "ip_neg=-32"));
    CHECK(has_line(b, "ip_hi=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "ip_zero=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "ip_imin=" + std::to_string(INT_MIN)));
    CHECK(has_line(b, "ip_neg1=" + std::to_string(-INT_MAX)));
    CHECK(has_line(b, "ip_min0=" + std::to_string(INT_MIN)));
    CHECK(has_line(b, "ip_near=" + std::to_string(INT_MAX)));
    CHECK(has_line(b, "ip_big=2147395600"));
    CHECK(has_line(b, "ip_sc=55"));
    CHECK(has_line(b, "ip_scmin=" + std::to_string(SCHAR_MIN)));
    CHECK(has_line(b, "ip_scneg=" + std::to_string(-SCHAR_MAX)));
    CHECK(has_line(b, "ip_uc=55"));
    CHECK(has_line(b, "ip_ucmax=" + std::to_string(UCHAR_MAX)));
    CHECK(has_line(b, "ip_u=32"));
    CHECK(has_line(b, "ip_unear=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ip_umax=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ip_uzero=" + std::to_string(UINT_MAX)));
    CHECK(has_line(b, "ip_f=20"));
    CHECK(has_line(b, "ip_b=1"));
    CHECK(has_line(b, "ip_bf=0"));
    CHECK(has_line(b, "ip_bt=1"));
    CHECK(has_line(b, "ip_ll=32"));
    CHECK(has_line(b, "gcd00=0"));

    std::string hi_out;
    REQUIRE(run_cmd({mod.string(), "ip-max1"}, &hi_out));
    CHECK(hi_out.find("ip_hi=" + std::to_string(INT_MAX)) != std::string::npos);

    auto expect_trap = [&](const char* arg, const char* cls, const char* msg) {
        std::string out, err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &out, &err));
        const std::string both = out + err;
        CHECK(both.find(std::string("CHECK ") + cls + ":") != std::string::npos);
        CHECK(both.find(msg) != std::string::npos);
        CHECK(out.find("wrapped=") == std::string::npos);
    };
    expect_trap("ip-ovf", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-sum", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-llovf", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-minovf", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-minsum", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-sc", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-part", "INT-SIGNED-OVF", "std::inner_product result does not fit in T");
    expect_trap("ip-uovf", "FUNC-CONTRACT", "std::inner_product result does not fit in T");
    expect_trap("ip-usum", "FUNC-CONTRACT", "std::inner_product result does not fit in T");
    expect_trap("ip-ucovf", "FUNC-CONTRACT", "std::inner_product result does not fit in T");
    expect_trap("ip-inv", "FUNC-CONTRACT", "std::inner_product last is before first");
    expect_trap("ip-null", "PTR-NULL-DEREF", "std::inner_product null second range");

    const fs::path ip_const = c.tmp / "ip_const.cpp";
    std::ofstream(ip_const) << "#include <numeric>\n"
                               "#include <climits>\n"
                               "constexpr int seq() {\n"
                               "  int a[4] = {1, 2, 3, 4};\n"
                               "  int b[4] = {1, 1, 1, 1};\n"
                               "  return std::inner_product(a, a + 4, b, 0) == 10 ? 1 : 0;\n"
                               "}\n"
                               "static_assert(seq() == 1);\n"
                               "constexpr int empty() {\n"
                               "  int a[1] = {7};\n"
                               "  return (std::inner_product(a, a, static_cast<int*>(nullptr), 3) == 3 && a[0] == 7) ? 1 : 0;\n"
                               "}\n"
                               "static_assert(empty() == 1);\n"
                               "constexpr int hi() {\n"
                               "  int a[1] = {INT_MAX};\n"
                               "  int b[1] = {1};\n"
                               "  return std::inner_product(a, a + 1, b, 0) == INT_MAX ? 1 : 0;\n"
                               "}\n"
                               "static_assert(hi() == 1);\n"
                               "constexpr int imin() {\n"
                               "  int a[1] = {INT_MIN};\n"
                               "  int b[1] = {1};\n"
                               "  return std::inner_product(a, a + 1, b, 0) == INT_MIN ? 1 : 0;\n"
                               "}\n"
                               "static_assert(imin() == 1);\n"
                               "constexpr int bits() {\n"
                               "  bool a[2] = {false, false};\n"
                               "  bool b[2] = {true, true};\n"
                               "  return std::inner_product(a, a + 2, b, false) == false ? 1 : 0;\n"
                               "}\n"
                               "static_assert(bits() == 1);\n"
                               "constexpr int big() {\n"
                               "  int a[1] = {46340};\n"
                               "  int b[1] = {46340};\n"
                               "  return std::inner_product(a, a + 1, b, 0) == 2147395600 ? 1 : 0;\n"
                               "}\n"
                               "static_assert(big() == 1);\n";
    CHECK(syntax_only(c.cxx, ip_const, true));
    CHECK(syntax_only(c.cxx, ip_const, false));

    const fs::path ip_bad = c.tmp / "ip_bad.cpp";
    std::ofstream(ip_bad) << "#include <numeric>\n"
                             "#include <climits>\n"
                             "constexpr int bad() {\n"
                             "  int a[1] = {INT_MAX};\n"
                             "  int b[1] = {2};\n"
                             "  return std::inner_product(a, a + 1, b, 0);\n"
                             "}\n"
                             "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_bad, true));
    CHECK_FALSE(syntax_only(c.cxx, ip_bad, false));

    const fs::path ip_sum = c.tmp / "ip_sum.cpp";
    std::ofstream(ip_sum) << "#include <numeric>\n"
                             "#include <climits>\n"
                             "constexpr int bad() {\n"
                             "  int a[1] = {1};\n"
                             "  int b[1] = {1};\n"
                             "  return std::inner_product(a, a + 1, b, INT_MAX);\n"
                             "}\n"
                             "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_sum, true));

    const fs::path ip_ubad = c.tmp / "ip_ubad.cpp";
    std::ofstream(ip_ubad) << "#include <numeric>\n"
                                  "#include <climits>\n"
                                  "constexpr unsigned bad() {\n"
                                  "  unsigned a[1] = {UINT_MAX};\n"
                                  "  unsigned b[1] = {2u};\n"
                                  "  return std::inner_product(a, a + 1, b, 0u);\n"
                                  "}\n"
                                  "constexpr unsigned v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_ubad, true));

    const fs::path ip_inv = c.tmp / "ip_inv.cpp";
    std::ofstream(ip_inv) << "#include <numeric>\n"
                             "constexpr int bad() {\n"
                             "  int a[2] = {4, 5};\n"
                             "  int b[2] = {6, 7};\n"
                             "  return std::inner_product(a + 1, a, b, 0);\n"
                             "}\n"
                             "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_inv, true));

    const fs::path ip_null = c.tmp / "ip_null.cpp";
    std::ofstream(ip_null) << "#include <numeric>\n"
                                  "constexpr int bad() {\n"
                                  "  int a[1] = {3};\n"
                                  "  return std::inner_product(a, a + 1, static_cast<int*>(nullptr), 0);\n"
                                  "}\n"
                                  "constexpr int v = bad();\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_null, true));

    const fs::path ip_mix = c.tmp / "ip_mix.cpp";
    std::ofstream(ip_mix) << "#include <numeric>\n"
                             "int main() {\n"
                             "  int a[2] = {1, 2};\n"
                             "  long b[2] = {3, 4};\n"
                             "  return static_cast<int>(std::inner_product(a, a + 2, b, 0));\n"
                             "}\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_mix, true));
    CHECK(syntax_only(c.cxx, ip_mix, false));

    const fs::path ip_init = c.tmp / "ip_init_type.cpp";
    std::ofstream(ip_init) << "#include <numeric>\n"
                                  "int main() {\n"
                                  "  int a[2] = {1, 2};\n"
                                  "  int b[2] = {3, 4};\n"
                                  "  return static_cast<int>(std::inner_product(a, a + 2, b, 0L));\n"
                                  "}\n";
    CHECK_FALSE(syntax_only(c.cxx, ip_init, true));
    CHECK(syntax_only(c.cxx, ip_init, false));

    const fs::path ip_mem = c.tmp / "ip_mem.cpp";
    std::ofstream(ip_mem) << "#include <climits>\n"
                             "#include <cstdio>\n"
                             "#include <cstdlib>\n"
                             "#include <cstring>\n"
                             "#include <numeric>\n"
                             "static int ga[2] = {2, 5};\n"
                             "static int gb[2] = {3, 6};\n"
                             "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
                             "  if (!ok) {\n"
                             "    std::fprintf(stderr, \"CHECK %s: %s\\n\", cls ? cls : \"\", msg ? msg : \"\");\n"
                             "    std::fprintf(stderr, \"mem=%d,%d,%d,%d\\n\", ga[0], ga[1], gb[0], gb[1]);\n"
                             "    std::fflush(stderr);\n"
                             "    std::abort();\n"
                             "  }\n"
                             "}\n"
                             "int main(int argc, char** argv) {\n"
                             "  const char* arg = (argc > 1 && argv[1]) ? argv[1] : \"ovf\";\n"
                             "  if (std::strcmp(arg, \"part\") == 0) {\n"
                             "    ga[0] = 2;\n"
                             "    ga[1] = INT_MAX;\n"
                             "    gb[0] = 3;\n"
                             "    gb[1] = 2;\n"
                             "    const int r = std::inner_product(ga, ga + 2, gb, 0);\n"
                             "    std::printf(\"wrapped=%d\\n\", r);\n"
                             "  } else if (std::strcmp(arg, \"sum\") == 0) {\n"
                             "    ga[0] = 1;\n"
                             "    gb[0] = 1;\n"
                             "    const int r = std::inner_product(ga, ga + 1, gb, INT_MAX);\n"
                             "    std::printf(\"wrapped=%d\\n\", r);\n"
                             "  } else if (std::strcmp(arg, \"inv\") == 0) {\n"
                             "    ga[0] = 4;\n"
                             "    ga[1] = 5;\n"
                             "    gb[0] = 6;\n"
                             "    gb[1] = 7;\n"
                             "    const int r = std::inner_product(ga + 1, ga, gb, 0);\n"
                             "    std::printf(\"wrapped=%d\\n\", r);\n"
                             "  } else if (std::strcmp(arg, \"null\") == 0) {\n"
                             "    ga[0] = 3;\n"
                             "    const int r = std::inner_product(ga, ga + 1, static_cast<int*>(nullptr), 0);\n"
                             "    std::printf(\"wrapped=%d\\n\", r);\n"
                             "  } else if (std::strcmp(arg, \"empty\") == 0) {\n"
                             "    ga[0] = 7;\n"
                             "    const int r = std::inner_product(ga, ga, static_cast<int*>(nullptr), 3);\n"
                             "    std::printf(\"empty=%d,%d\\n\", r, ga[0]);\n"
                             "    return 0;\n"
                             "  } else if (std::strcmp(arg, \"max1\") == 0) {\n"
                             "    int a[1] = {INT_MAX};\n"
                             "    int b[1] = {1};\n"
                             "    std::printf(\"ip_hi=%d\\n\", std::inner_product(a, a + 1, b, 0));\n"
                             "    return 0;\n"
                             "  } else {\n"
                             "    ga[0] = INT_MAX;\n"
                             "    gb[0] = 2;\n"
                             "    const int r = std::inner_product(ga, ga + 1, gb, 0);\n"
                             "    std::printf(\"wrapped=%d\\n\", r);\n"
                             "  }\n"
                             "  return 0;\n"
                             "}\n";
#ifdef _WIN32
    const fs::path ip_mem_exe = c.tmp / "ip_mem.exe";
#else
    const fs::path ip_mem_exe = c.tmp / "ip_mem";
#endif
    std::vector<std::string> mem_build{c.cxx, "-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-g", "-O1", "-w", "-isystem",
                                       models.string(), ip_mem.string(), "-o", ip_mem_exe.string()};
    REQUIRE(run_cmd(mem_build));
    std::string mout, merr;
    CHECK_FALSE(run_cmd({ip_mem_exe.string(), "ovf"}, &mout, &merr));
    std::string mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::inner_product result does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=" + std::to_string(INT_MAX) + ",5,2,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ip_mem_exe.string(), "part"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::inner_product result does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=2," + std::to_string(INT_MAX) + ",3,2") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ip_mem_exe.string(), "sum"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK INT-SIGNED-OVF: std::inner_product result does not fit in T") != std::string::npos);
    CHECK(mboth.find("mem=1,5,1,6") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ip_mem_exe.string(), "inv"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK FUNC-CONTRACT: std::inner_product last is before first") != std::string::npos);
    CHECK(mboth.find("mem=4,5,6,7") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    CHECK_FALSE(run_cmd({ip_mem_exe.string(), "null"}, &mout, &merr));
    mboth = mout + merr;
    CHECK(mboth.find("CHECK PTR-NULL-DEREF: std::inner_product null second range") != std::string::npos);
    CHECK(mout.find("wrapped=") == std::string::npos);
    std::string mok;
    REQUIRE(run_cmd({ip_mem_exe.string(), "max1"}, &mok));
    CHECK(mok.find("ip_hi=" + std::to_string(INT_MAX)) != std::string::npos);
    REQUIRE(run_cmd({ip_mem_exe.string(), "empty"}, &mok));
    CHECK(mok.find("empty=3,7") != std::string::npos);

    const fs::path ip_trap = c.tmp / "ip_trap.cpp";
    std::ofstream(ip_trap) << "#include <climits>\n"
                                  "#include <cstdio>\n"
                                  "#include <numeric>\n"
                                  "extern \"C\" void __prism_check(int, const char*, const char*) {}\n"
                                  "int main() {\n"
                                  "  int a[1] = {INT_MAX};\n"
                                  "  int b[1] = {2};\n"
                                  "  const int r = std::inner_product(a, a + 1, b, 0);\n"
                                  "  std::printf(\"wrapped=%d\\n\", r);\n"
                                  "  return 0;\n"
                                  "}\n";
#ifdef _WIN32
    const fs::path ip_trap_exe = c.tmp / "ip_trap.exe";
#else
    const fs::path ip_trap_exe = c.tmp / "ip_trap";
#endif
    std::vector<std::string> trap_build{c.cxx, "-std=c++23", "-g", "-O1", "-w", "-isystem", models.string(),
                                        ip_trap.string(), "-o", ip_trap_exe.string()};
    REQUIRE(run_cmd(trap_build));
    std::string tout, terr;
    CHECK_FALSE(run_cmd({ip_trap_exe.string()}, &tout, &terr));
    CHECK(tout.find("wrapped=") == std::string::npos);
}
