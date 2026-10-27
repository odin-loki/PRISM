// The Windows PIR <string.h> shim is declarations only. Read it as text.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("string shim: memset, memcpy, and strlen are declarations only") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "shim" / "string.h");
    REQUIRE(!src.empty());
    CHECK(src.find("void* memset(void*, int, size_t);") != std::string::npos);
    CHECK(src.find("void* memcpy(void*, const void*, size_t);") != std::string::npos);
    CHECK(src.find("size_t strlen(const char*);") != std::string::npos);
    // A brace body would define the symbol in the scanned TU and hide string.cpp.
    CHECK(src.find('{') == std::string::npos);
    CHECK(src.find("PROVED") == std::string::npos);
}
