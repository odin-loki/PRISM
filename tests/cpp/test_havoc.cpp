// AFL++ interesting-value havoc: host havoc.cpp and CUDA mutate.cu stay aligned.
// Port of tests/test_simdmut.py and tests/test_cuda.py (source locks and
// behavioural checks; Python ctypes / simdmut.py loader tests deleted).

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/simd.hpp"
#include "prism/taxonomy.hpp"

#ifdef PRISM_HAS_CUDA
#include "prism/cuda_mutate.h"
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr std::array<int, 9> kAfl8{-128, -1, 0, 1, 16, 32, 64, 100, 127};
constexpr std::array<int, 10> kAfl16{-32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767};
constexpr std::array<int32_t, 9> kAfl32{-2147483648, -100663046, -32769, 32768, 65535, 65536,
                                        100663045, 2139095040, 2147483647};

void overlay_le(uint8_t* data, size_t n, size_t i, uint32_t raw, int width) {
    for (int k = 0; k < width; ++k) {
        if (i + static_cast<size_t>(k) >= n) break;
        data[i + static_cast<size_t>(k)] = static_cast<uint8_t>(raw >> (8 * k));
    }
}

std::vector<int> brace_ints(const std::string& src, const std::string& name) {
    auto i = src.find(name);
    REQUIRE(i != std::string::npos);
    auto a = src.find('{', i);
    auto b = src.find('}', a);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    std::string body = src.substr(a, b - a + 1);
    body = std::regex_replace(body, std::regex("std::numeric_limits<int32_t>::min\\(\\)"), "-2147483648");
    body = std::regex_replace(body, std::regex(R"(\(int32_t\)\s*0x80000000)"), "-2147483648");
    body = std::regex_replace(body, std::regex("/\\*.*?\\*/", std::regex::extended), "");
    std::vector<int> out;
    std::regex num(R"(-?\d+|0x[0-9A-Fa-f]+)");
    for (std::sregex_iterator it(body.begin(), body.end(), num), end; it != end; ++it)
        out.push_back(std::stoi(it->str(), nullptr, it->str().find("0x") == 0 ? 0 : 10));
    return out;
}

std::vector<int> overlay_widths(const std::string& src) {
    std::vector<int> w;
    std::regex re(R"(overlay_le\s*\([^;]+,\s*([124])\s*\))");
    for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it)
        w.push_back(std::stoi((*it)[1].str()));
    return w;
}

prism::RunReport rep_with(std::initializer_list<prism::StageResult> stages) {
    prism::RunReport r;
    r.root = "x";
    for (auto s : stages) r.stages.push_back(s);
    return r;
}

}  // namespace

TEST_CASE("havoc: INTERESTING tables match AFL++ constants") {
    CHECK(kAfl8[0] == -128);
    CHECK(kAfl32[0] == -2147483648);
}

TEST_CASE("havoc: overlay_le width 1/2/4 and end clipping") {
    uint8_t buf[4];
    std::memset(buf, 0, sizeof buf);
    overlay_le(buf, 4, 0, static_cast<uint32_t>(static_cast<uint8_t>(-128)), 1);
    CHECK(buf[0] == 0x80);
    overlay_le(buf, 4, 1, 127, 1);
    CHECK(buf[1] == 127);

    std::memset(buf, 0, sizeof buf);
    overlay_le(buf, 4, 0, 256, 2);
    CHECK(buf[0] == 0);
    CHECK(buf[1] == 1);
    overlay_le(buf, 4, 0, -32768, 2);
    CHECK(buf[0] == 0);
    CHECK(buf[1] == 0x80);

    std::memset(buf, 0, 8);
    overlay_le(buf, 8, 0, 32768, 4);
    CHECK(std::memcmp(buf, "\x00\x80\x00\x00", 4) == 0);
    overlay_le(buf, 8, 0, static_cast<uint32_t>(-2147483648LL), 4);
    CHECK(std::memcmp(buf, "\x00\x00\x00\x80", 4) == 0);
    overlay_le(buf, 8, 0, 2139095040u, 4);
    CHECK(static_cast<int32_t>(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)) == 2139095040);

    uint8_t clip[2] = {0xFF, 0xFF};
    overlay_le(clip, 2, 1, 0x01020304u, 4);
    CHECK(clip[0] == 0xFF);
    CHECK(clip[1] == 0x04);
}

TEST_CASE("havoc: cpu havoc preserves length and mutates") {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t orig[8];
    std::memcpy(orig, buf, 8);
    prism::havoc(buf, 8, 0xC0FFEEULL);
    bool changed = false;
    for (int i = 0; i < 8; ++i)
        if (buf[i] != orig[i]) changed = true;
    CHECK(changed);
    CHECK_FALSE(prism::laws::is_proof(prism::laws::CLEAN));
}

#ifdef PRISM_HAS_CUDA
TEST_CASE("havoc: cuda havoc returns 0 or honest device failure never a proof") {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int rc = prism_cuda_havoc(buf, 8, 1, 1);
    CHECK(rc <= 0);
    CHECK(rc != 1);
}
#endif

TEST_CASE("havoc: havoc.cpp tables and overlay widths match AFL++") {
    const auto src = slurp(repo() / "src" / "prism" / "havoc.cpp");
    auto i8 = brace_ints(src, "interesting8");
    auto i16 = brace_ints(src, "interesting16");
    auto i32 = brace_ints(src, "interesting32");
    REQUIRE_EQ(i8.size(), kAfl8.size());
    REQUIRE_EQ(i16.size(), kAfl16.size());
    REQUIRE_EQ(i32.size(), kAfl32.size());
    for (std::size_t i = 0; i < kAfl8.size(); ++i) CHECK(i8[i] == kAfl8[static_cast<std::size_t>(i)]);
    for (std::size_t i = 0; i < kAfl16.size(); ++i) CHECK(i16[i] == kAfl16[static_cast<std::size_t>(i)]);
    for (std::size_t i = 0; i < kAfl32.size(); ++i) CHECK(i32[i] == kAfl32[static_cast<std::size_t>(i)]);
    CHECK(src.find("std::numeric_limits<int32_t>::min()") != std::string::npos);
    CHECK(overlay_widths(src) == std::vector<int>{1, 2, 4});
    CHECK(src.find("case 4:") != std::string::npos);
    CHECK(src.find("case 5:") != std::string::npos);
    CHECK(src.find("case 6:") != std::string::npos);
}

TEST_CASE("havoc: mutate.cu tables match havoc.cpp") {
    const auto cu = slurp(repo() / "src" / "cuda" / "mutate.cu");
    const auto host = slurp(repo() / "src" / "prism" / "havoc.cpp");
    for (const char* name : {"interesting8", "interesting16", "interesting32"}) {
        CHECK(brace_ints(cu, name) == brace_ints(host, name));
    }
    CHECK(overlay_widths(cu) == std::vector<int>{1, 2, 4});
    CHECK(overlay_widths(cu) == overlay_widths(host));
    CHECK(cu.find("kind == 4") != std::string::npos);
    CHECK(cu.find("kind == 5") != std::string::npos);
    CHECK(cu.find("kind == 6") != std::string::npos);
    CHECK(cu.find("NOTRUN") != std::string::npos);
    CHECK(cu.find("Missing nvcc") != std::string::npos);
    CHECK(cu.find("CLEAN") == std::string::npos);
}

TEST_CASE("havoc: CMake CUDA skip is WARNING NOTRUN not STATUS CLEAN") {
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    const char* warn = "CUDA kernel skipped (NOTRUN, not a silent disable)";
    CHECK(cmake.find(warn) != std::string::npos);
    CHECK(cmake.find("CUDA kernel skipped (CLEAN") == std::string::npos);
    CHECK(cmake.find(std::string("message(STATUS \"") + warn) == std::string::npos);
    std::regex cuda_block(R"(if\s*\(\s*PRISM_CUDA\s*\).*(?:\nendif\(\)\s*\n\s*\n|\nendif\(\)\s*\nif\(PRISM_LLAMA))",
                          std::regex::extended);
    std::smatch m;
    CHECK(std::regex_search(cmake, m, cuda_block));
    const std::string block = m[0].str();
    CHECK(block.find("CMAKE_CXX_COMPILER_ID MATCHES \"Clang\"") != std::string::npos);
    CHECK(block.find("enable_language(CUDA)") != std::string::npos);
    CHECK(block.find(warn) != std::string::npos);
    CHECK(block.find("message(WARNING") != std::string::npos);
    CHECK(prism::laws::NOTRUN != prism::laws::CLEAN);
}

TEST_CASE("havoc: NOTRUN fuzz findings do not COVER taxonomy rows") {
    prism::StageResult fuzz;
    fuzz.name = "fuzz";
    fuzz.status = "NOTRUN";
    fuzz.detail = "nvcc unusable / no GPU";
    prism::Finding f;
    f.stage = "fuzz";
    f.status = prism::laws::NOTRUN;
    f.cls = "INT-SIGNED-OVF";
    f.message = "CUDA kernel skipped (NOTRUN, not a silent disable)";
    f.strength = prism::laws::STRENGTH_FINDS;
    fuzz.findings = {f};
    auto rows = prism::coverage_from_report(rep_with({fuzz}));
    const prism::TaxonomyRow* row = nullptr;
    for (auto& r : rows)
        if (r.id == "INT-SIGNED-OVF") row = &r;
    REQUIRE(row != nullptr);
    CHECK(row->verdict == "GAP");
    CHECK(row->verdict != "COVERED");
}
