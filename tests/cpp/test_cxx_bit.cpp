// Differential test of the <bit> model header (src/prism/pir/models/cxx/bit).
// Same shape as tests/cpp/test_cxx_initializer_list.cpp: the trace is compiled
// twice, once against libstdc++ and once with -isystem on the model directory
// so `#include <bit>` is this header. CMake's prism_tests source list is
// explicit (not a glob).
//
// countl_zero(0) is the digit count of unsigned. popcount(0xA) is 2. The
// header text does not contain a count-leading-zeros builtin call on zero.
// countl_one(0) and countr_one(0) are 0. Each of those names is one template.
// byteswap is declared for an unsigned value. bit_cast stays omitted. The
// header does not call a byte-swap builtin.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "bit"; }

struct BitCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

BitCtx& ctx() {
    static BitCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <bit>\n#include <limits>\n"
                                  "int main() {\n"
                                  "  return std::countl_zero(0u) == std::numeric_limits<unsigned>::digits &&\n"
                                  "         std::countl_one(0u) == 0 &&\n"
                                  "         std::countr_one(0u) == 0 &&\n"
                                  "         std::countr_one(2u) == 0 &&\n"
                                  "         std::countl_one(~0u) == std::numeric_limits<unsigned>::digits &&\n"
                                  "         std::countr_one(~0u) == std::numeric_limits<unsigned>::digits &&\n"
                                  "         std::popcount(0xAu) == 2 ? 0 : 1;\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("bit_trace.cpp"), "bit_trace", cxxh::asan_flags(), with_models);
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

bool clz0_is_width(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        int z = 0;
        int d = 0;
        if (std::sscanf(line.c_str(), "unsigned clz0 %d digits %d", &z, &d) == 2) return z == d && d > 0;
    }
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int occurrences(const std::string& text, const std::string& needle) {
    int n = 0;
    std::size_t p = 0;
    while ((p = text.find(needle, p)) != std::string::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

}  // namespace

TEST_CASE("cxx bit model: header is a file and does not call clz on zero") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_BIT") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("__builtin_clz(0)") == std::string::npos);
    CHECK(text.find("__builtin_ctz(0)") == std::string::npos);
    CHECK(text.find("__builtin_clz") == std::string::npos);
    CHECK(text.find("__builtin_ctz") == std::string::npos);
    CHECK(occurrences(text, "constexpr int countl_zero(") == 1);
    CHECK(occurrences(text, "constexpr int countr_zero(") == 1);
    CHECK(occurrences(text, "constexpr int countl_one(") == 1);
    CHECK(occurrences(text, "constexpr int countr_one(") == 1);
    CHECK(occurrences(text, "if (__x == 0) return _Nd;") == 2);
    CHECK(occurrences(text, "if (__x == 0) return 0;") == 2);
    CHECK(text.find("if ((__x & _Tp(1)) == 0) return 0;") != std::string::npos);
    CHECK(text.find("for (int __i = 0; __i < _Nd; ++__i)") != std::string::npos);
    CHECK(text.find("do not scan off the width") != std::string::npos);
    CHECK(text.find("not PROVED and not CLEAN") != std::string::npos);
    CHECK(text.find("__builtin_bswap") == std::string::npos);
    CHECK(text.find("__builtin_bit_cast") == std::string::npos);
    CHECK(text.find("byteswap and bit_cast are not declared") == std::string::npos);
    CHECK(text.find("constexpr _Tp byteswap(_Tp __x)") != std::string::npos);
    CHECK(text.find("bit_cast is not declared") != std::string::npos);
    CHECK(text.find("bit_cast(") == std::string::npos);
    CHECK(text.find("__cpp_lib_byteswap 202110L") != std::string::npos);
    CHECK(text.find("sizeof(_Tp) == 1") != std::string::npos);
    CHECK(text.find("bit_ceil: smallest power of two is not representable") != std::string::npos);
    CHECK(text.find("if (__x == 0) return _Nd;") != std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("#include <memory") == std::string::npos);
    CHECK(text.find("#include <deque") == std::string::npos);
    CHECK(text.find("#include <list") == std::string::npos);
}

TEST_CASE("cxx bit model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxbit-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx bit model: countl_zero(0) is the width and popcount(0xA) is 2") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "bit_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(clz0_is_width(b));
    CHECK(has_line(b, "unsigned popA 2"));
    CHECK(has_line(b, "counts 1 1 1"));
    CHECK(has_line(b, "round 1 1 1"));
    CHECK(has_line(b, "swap0 0"));
    CHECK(has_line(b, "swap8 ab"));
    CHECK(has_line(b, "swap16 3412"));
    CHECK(has_line(b, "swap32 78563412"));
    CHECK(has_line(b, "swap1 1000000"));
    CHECK(has_line(b, "swap64 efcdab8967452301"));
    CHECK(has_line(b, "swaprt 1 1 1 1"));
    CHECK(has_line(b, "ones u8 clo0 0 clohi 1 clorun 4 cloall 8 cro0 0 crolow 0 crohi 0 crorun 4 croall 8"));
    CHECK(has_line(b, "ones u16 clo0 0 clohi 1 clorun 4 cloall 16 cro0 0 crolow 0 crohi 0 crorun 4 croall 16"));
    CHECK(has_line(b, "ones u32 clo0 0 clohi 1 clorun 4 cloall 32 cro0 0 crolow 0 crohi 0 crorun 4 croall 32"));
    CHECK(has_line(b, "ones unsigned clo0 0 clohi 1 clorun 4 cloall 32 cro0 0 crolow 0 crohi 0 crorun 4 croall 32"));
    CHECK(has_line(b, "ones u64 clo0 0 clohi 1 clorun 4 cloall 64 cro0 0 crolow 0 crohi 0 crorun 4 croall 64"));
}
