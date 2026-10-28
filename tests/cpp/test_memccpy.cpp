// memccpy model. Reads the model and the string contract harness as text.
// Does not compile them. docs/finish-notes/memccpy.md

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::string function_body(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i - open + 1));
        }
    }
    return {};
}

std::size_t count_of(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("memccpy model: null returns before access, n == 0 does not read, copy stops at c") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 8);
    CHECK(count_of(src, "PROVED") == 8);
    CHECK(count_of(src, "CLEAN") == 8);

    const auto memrchr_at = src.find("void *memrchr(const void *s, int c, size_t n) {");
    const auto memccpy_at = src.find("void *memccpy(void *d, const void *s, int c, size_t n) {");
    const auto malloc_at = src.find("void *malloc(size_t n);");
    REQUIRE(memrchr_at != std::string::npos);
    REQUIRE(memccpy_at != std::string::npos);
    REQUIRE(malloc_at != std::string::npos);
    CHECK(memrchr_at < memccpy_at);
    CHECK(memccpy_at < malloc_at);
    CHECK(src.find("void *memccpy(", memccpy_at + 1) == std::string::npos);

    const auto note_at = src.rfind("/*", memccpy_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > memrchr_at);
    const auto note = src.substr(note_at, memccpy_at - note_at);
    CHECK(note.find("until c is copied, or n bytes are") != std::string::npos);
    CHECK(note.find("n == 0 returns a null pointer and does not read d or s.") != std::string::npos);
    CHECK(note.find("A null d or a null s with n > 0 is STR-NULL-ARG") != std::string::npos);
    CHECK(note.find("before any read or write.") != std::string::npos);
    CHECK(note.find("does not read or write index i for i >= n.") != std::string::npos);
    CHECK(note.find("If c is copied, the return is a pointer to the next byte of d.") != std::string::npos);
    CHECK(note.find("If c is not found, the return is a null pointer.") != std::string::npos);
    CHECK(note.find("This is not a proof of the copy.") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto body = function_body(src, "void *memccpy(void *d, const void *s, int c, size_t n)");
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);
    CHECK(body ==
          "{\n"
          "    if (n == 0) return 0;\n"
          "    __prism_check(d != 0, \"STR-NULL-ARG\", \"memccpy() called with unchecked pointer parameter d\");\n"
          "    if (d == 0) return 0;\n"
          "    __prism_check(s != 0, \"STR-NULL-ARG\", \"memccpy() called with unchecked pointer parameter s\");\n"
          "    if (s == 0) return 0;\n"
          "    unsigned char *dd = (unsigned char *)d;\n"
          "    const unsigned char *ss = (const unsigned char *)s;\n"
          "    const unsigned char want = (unsigned char)(c & 0xff);\n"
          "    for (size_t i = 0; i < n; ++i) {\n"
          "        dd[i] = ss[i];\n"
          "        if (dd[i] == want) return (void *)(dd + i + 1);\n"
          "    }\n"
          "    return 0;\n"
          "}");

    const auto zero = body.find("if (n == 0) return 0;");
    const auto check_d = body.find(
        "__prism_check(d != 0, \"STR-NULL-ARG\", \"memccpy() called with unchecked pointer parameter d\");");
    const auto early_d = body.find("if (d == 0) return 0;");
    const auto check_s = body.find(
        "__prism_check(s != 0, \"STR-NULL-ARG\", \"memccpy() called with unchecked pointer parameter s\");");
    const auto early_s = body.find("if (s == 0) return 0;");
    const auto loop = body.find("for (size_t i = 0; i < n; ++i)");
    REQUIRE(zero != std::string::npos);
    REQUIRE(check_d != std::string::npos);
    REQUIRE(early_d != std::string::npos);
    REQUIRE(check_s != std::string::npos);
    REQUIRE(early_s != std::string::npos);
    REQUIRE(loop != std::string::npos);
    CHECK(zero < check_d);
    CHECK(check_d < early_d);
    CHECK(early_d < check_s);
    CHECK(check_s < early_s);
    CHECK(early_s < loop);
    const auto before = body.substr(0, loop);
    CHECK(before.find("dd[") == std::string::npos);
    CHECK(before.find("ss[") == std::string::npos);
    CHECK(before.find("[i]") == std::string::npos);
    CHECK(body.find("dd[n]") == std::string::npos);
    CHECK(body.find("ss[n]") == std::string::npos);
    CHECK(body.find("i >= n") == std::string::npos);
    CHECK(body.find("i <= n") == std::string::npos);
    CHECK(body.find("return (void *)(dd + i + 1);") != std::string::npos);
    CHECK(body.find("__prism_memcpy") == std::string::npos);

    const auto dup = function_body(src, "char *strndup(const char *s, size_t n)");
    CHECK(dup ==
          "{\n"
          "    __prism_check(s != 0, \"STR-NULL-ARG\", \"strndup() called with unchecked pointer parameter s\");\n"
          "    if (s == 0) return 0;\n"
          "    if (n == 0) {\n"
          "        char *p = (char *)malloc(1);\n"
          "        if (!p) return 0;\n"
          "        p[0] = 0;\n"
          "        return p;\n"
          "    }\n"
          "    size_t len = 0;\n"
          "    while (len < n && s[len]) ++len;\n"
          "    if (len == ~(size_t)0) {\n"
          "        __prism_check(0, \"MEM-OOB-WRITE\", \"strndup length does not fit in size_t\");\n"
          "        __builtin_trap();\n"
          "    }\n"
          "    char *p = (char *)malloc(len + 1);\n"
          "    if (!p) return 0;\n"
          "    for (size_t i = 0; i < len; ++i) p[i] = s[i];\n"
          "    p[len] = 0;\n"
          "    return p;\n"
          "}");
    const auto fit = dup.find(
        "__prism_check(0, \"MEM-OOB-WRITE\", \"strndup length does not fit in size_t\");");
    const auto trap = dup.find("__builtin_trap();");
    const auto alloc = dup.find("char *p = (char *)malloc(len + 1);");
    REQUIRE(fit != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(alloc != std::string::npos);
    CHECK(fit < trap);
    CHECK(trap < alloc);

    for (const char* sig : {"size_t strlen(const char *s) {", "size_t strnlen(const char *s, size_t n) {",
                            "char *strcpy(char *d, const char *s) {", "char *strncpy(char *d, const char *s, size_t n) {",
                            "char *strcat(char *d, const char *s) {", "char *strncat(char *d, const char *s, size_t n) {",
                            "int strcmp(const char *a, const char *b) {", "int strncmp(const char *a, const char *b, size_t n) {",
                            "char *strchr(const char *s, int c) {", "char *strrchr(const char *s, int c) {",
                            "size_t strspn(const char *s, const char *accept) {",
                            "size_t strcspn(const char *s, const char *accept) {",
                            "char *strstr(const char *s, const char *needle) {",
                            "char *strpbrk(const char *s, const char *accept) {",
                            "void *memcpy(void *d, const void *s, size_t n) {", "void *memmove(void *d, const void *s, size_t n) {",
                            "void *memset(void *d, int c, size_t n) {", "int memcmp(const void *a, const void *b, size_t n) {",
                            "void *memchr(const void *s, int c, size_t n) {", "void *memrchr(const void *s, int c, size_t n) {",
                            "char *strdup(const char *s) {", "char *strndup(const char *s, size_t n) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("memccpy contracts: harness calls memccpy and yml expects no copy") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);

    const auto body = function_body(cpp, "int memcpy_true(int n, int j)");
    REQUIRE(!body.empty());
    CHECK(body.find("void *r = memcpy(d, s, (size_t)n);") != std::string::npos);
    CHECK(body.find("assert(r == d);") != std::string::npos);
    CHECK(body.find("assert(d[j] == (j < n ? s[j] : d0[j]));") != std::string::npos);
    CHECK(body.find("(void)memccpy(extra, s, 'a', (size_t)n);") != std::string::npos);
    CHECK(body.find("assert(memccpy") == std::string::npos);
    CHECK(body.find("memccpy(extra, s, 'a', (size_t)n) ==") == std::string::npos);

    CHECK(yml.find("memcpy_true: true") != std::string::npos);
    CHECK(yml.find("memcpy_overlap_false: false") != std::string::npos);
    CHECK(yml.find("memccpy") == std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_memccpy.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_strndup.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_stdio_buffer.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_memrchr.cpp") != std::string::npos);
}
