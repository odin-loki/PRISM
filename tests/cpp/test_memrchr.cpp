// memrchr model. Reads the model and the string contract harness as text.
// Does not compile them. docs/finish-notes/memrchr.md

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

// Statements inside `sig`, comments and blank lines dropped.
std::vector<std::string> statements(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    if (at == std::string::npos) return {};
    const auto open = at + sig.size() - 1;
    if (open >= src.size() || src[open] != '{') return {};
    int depth = 0;
    std::string interior;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
            if (depth == 1) continue;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) break;
        }
        if (depth >= 1) interior.push_back(src[i]);
    }
    std::vector<std::string> lines;
    std::string cur;
    auto flush = [&] {
        auto t = trim(cur);
        cur.clear();
        if (t.empty() || t[0] == '/' || t[0] == '*') return;
        lines.push_back(std::move(t));
    };
    for (char c : interior) {
        if (c == '\n') flush();
        else cur.push_back(c);
    }
    flush();
    return lines;
}

std::size_t count_of(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("memrchr model: last byte, zero length returns a null pointer, null does not index") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 5);
    CHECK(count_of(src, "PROVED") == 5);
    CHECK(count_of(src, "CLEAN") == 5);

    const auto memchr_at = src.find("void *memchr(");
    const auto memrchr_at = src.find("void *memrchr(");
    const auto malloc_at = src.find("void *malloc(");
    const auto strdup_at = src.find("char *strdup(");
    REQUIRE(memchr_at != std::string::npos);
    REQUIRE(memrchr_at != std::string::npos);
    REQUIRE(malloc_at != std::string::npos);
    REQUIRE(strdup_at != std::string::npos);
    CHECK(memchr_at < memrchr_at);
    CHECK(memrchr_at < malloc_at);
    CHECK(malloc_at < strdup_at);
    CHECK(src.find("void *memchr(", memchr_at + 1) == std::string::npos);
    CHECK(src.find("void *memrchr(", memrchr_at + 1) == std::string::npos);

    const auto note = src.substr(memchr_at, memrchr_at - memchr_at);
    CHECK(note.find("n == 0 returns a null pointer and does not read.") != std::string::npos);
    CHECK(note.find("STR-NULL-ARG") != std::string::npos);
    CHECK(note.find("does not index s") != std::string::npos);
    CHECK(note.find("from the last byte backward") != std::string::npos);
    CHECK(note.find("only those n bytes") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const std::string mem_sig = "void *memchr(const void *s, int c, size_t n) {";
    const std::string r_sig = "void *memrchr(const void *s, int c, size_t n) {";
    CHECK(statements(src, mem_sig) == std::vector<std::string>{
                                          "if (n == 0) return 0;",
                                          "__prism_check(s != 0, \"STR-NULL-ARG\", \"memchr() called with unchecked pointer parameter s\");",
                                          "const unsigned char *x = (const unsigned char *)s;",
                                          "const unsigned char want = (unsigned char)(c & 0xff);",
                                          "for (size_t i = 0; i < n; i++)",
                                          "if (x[i] == want) return (void *)(x + i);",
                                          "return 0;",
                                      });
    CHECK(statements(src, r_sig) == std::vector<std::string>{
                                        "if (n == 0) return 0;",
                                        "__prism_check(s != 0, \"STR-NULL-ARG\", \"memrchr() called with unchecked pointer parameter s\");",
                                        "if (s == 0) return 0;",
                                        "const unsigned char *x = (const unsigned char *)s;",
                                        "const unsigned char want = (unsigned char)(c & 0xff);",
                                        "size_t i = n;",
                                        "while (i > 0) {",
                                        "--i;",
                                        "if (x[i] == want) return (void *)(x + i);",
                                        "}",
                                        "return 0;",
                                    });

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
                            "void *memchr(const void *s, int c, size_t n) {", "char *strdup(const char *s) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("memrchr contracts: harness calls memrchr") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);
    CHECK(cpp.find("int memrchr_true(") != std::string::npos);
    CHECK(cpp.find("int memrchr_first_false(") != std::string::npos);
    CHECK(cpp.find("assert(memrchr(s, c, 0) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(memrchr(dup, 'a', 4) == dup + 2);") != std::string::npos);
    CHECK(cpp.find("assert(memrchr(dup, 'c', 4) == dup + 3);") != std::string::npos);
    CHECK(cpp.find("assert(memrchr(dup, 'z', 4) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(memrchr(s, 'a', 4) == s);") != std::string::npos);
    CHECK(count_of(cpp, "memrchr(") >= 1);
}

TEST_CASE("memrchr contracts: yml has a true task, a false task, and FUNC-CONTRACT") {
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: string_contracts.cpp") != std::string::npos);
    CHECK(yml.find("language: C") != std::string::npos);
    CHECK(yml.find("property: libc-contract") != std::string::npos);
    CHECK(yml.find("memrchr_true: true") != std::string::npos);
    CHECK(yml.find("memrchr_first_false: false") != std::string::npos);
    CHECK(yml.find("memrchr_first_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(yml.find("memchr_true: true") != std::string::npos);
    CHECK(yml.find("memchr_oob_false: \"MEM-OOB-READ\"") != std::string::npos);

    const auto expected = yml.find("expected:");
    const auto classes = yml.find("expect_class:");
    REQUIRE(expected != std::string::npos);
    REQUIRE(classes != std::string::npos);
    CHECK(classes > expected);
    const auto exp = yml.substr(expected, classes - expected);
    const auto cls = yml.substr(classes);
    CHECK(exp.find("memrchr_true: true") != std::string::npos);
    CHECK(exp.find("memrchr_first_false: false") != std::string::npos);
    CHECK(cls.find("memrchr_first_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(exp.find("memrchr_first_false: \"FUNC-CONTRACT\"") == std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_memrchr.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_strstr.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_strspn.cpp") != std::string::npos);
}
