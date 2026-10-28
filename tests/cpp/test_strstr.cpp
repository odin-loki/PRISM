// strstr and strpbrk models. Reads the model and the string contract harness
// as text. Does not compile them. docs/finish-notes/strstr.md

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

TEST_CASE("strstr model: first match, empty needle, null returns a null pointer without a scan") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 4);
    CHECK(count_of(src, "PROVED") == 4);
    CHECK(count_of(src, "CLEAN") == 4);

    const auto strcspn_at = src.find("size_t strcspn(");
    const auto strstr_at = src.find("char *strstr(");
    const auto strpbrk_at = src.find("char *strpbrk(");
    const auto memcpy_at = src.find("void *memcpy(");
    REQUIRE(strcspn_at != std::string::npos);
    REQUIRE(strstr_at != std::string::npos);
    REQUIRE(strpbrk_at != std::string::npos);
    REQUIRE(memcpy_at != std::string::npos);
    CHECK(strcspn_at < strstr_at);
    CHECK(strstr_at < strpbrk_at);
    CHECK(strpbrk_at < memcpy_at);
    CHECK(src.find("char *strstr(", strstr_at + 1) == std::string::npos);
    CHECK(src.find("char *strpbrk(", strpbrk_at + 1) == std::string::npos);

    const auto str_note = src.substr(strcspn_at, strstr_at - strcspn_at);
    CHECK(str_note.find("first match of needle in s") != std::string::npos);
    CHECK(str_note.find("An empty needle returns s and does not scan s.") != std::string::npos);
    CHECK(str_note.find("does not read past the terminator") != std::string::npos);
    CHECK(str_note.find("without a scan") != std::string::npos);
    CHECK(str_note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto pbrk_note = src.substr(strstr_at, strpbrk_at - strstr_at);
    CHECK(pbrk_note.find("first byte of s that occurs in accept") != std::string::npos);
    CHECK(pbrk_note.find("An empty accept returns a null pointer.") != std::string::npos);
    CHECK(pbrk_note.find("does not read past the terminator") != std::string::npos);
    CHECK(pbrk_note.find("without a scan") != std::string::npos);
    CHECK(pbrk_note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const std::string str_sig = "char *strstr(const char *s, const char *needle) {";
    const std::string pbrk_sig = "char *strpbrk(const char *s, const char *accept) {";
    CHECK(statements(src, str_sig) == std::vector<std::string>{
                                          "__prism_check(s != 0, \"STR-NULL-ARG\", \"strstr() called with unchecked pointer parameter s\");",
                                          "if (s == 0) return 0;",
                                          "__prism_check(needle != 0, \"STR-NULL-ARG\", \"strstr() called with unchecked pointer parameter needle\");",
                                          "if (needle == 0) return 0;",
                                          "if (!uc(needle, 0)) return (char *)s;",
                                          "size_t i = 0;",
                                          "while (uc(s, i)) {",
                                          "size_t j = 0;",
                                          "while (uc(needle, j) != 0 && uc(s, i + j) != 0 && uc(s, i + j) == uc(needle, j)) ++j;",
                                          "if (!uc(needle, j)) return (char *)(s + i);",
                                          "if (!uc(s, i + j)) return 0;",
                                          "++i;",
                                          "}",
                                          "return 0;",
                                      });
    CHECK(statements(src, pbrk_sig) == std::vector<std::string>{
                                           "__prism_check(s != 0, \"STR-NULL-ARG\", \"strpbrk() called with unchecked pointer parameter s\");",
                                           "if (s == 0) return 0;",
                                           "__prism_check(accept != 0, \"STR-NULL-ARG\", \"strpbrk() called with unchecked pointer parameter accept\");",
                                           "if (accept == 0) return 0;",
                                           "if (!uc(accept, 0)) return 0;",
                                           "size_t i = 0;",
                                           "while (uc(s, i)) {",
                                           "size_t j = 0;",
                                           "while (uc(accept, j) != 0 && uc(accept, j) != uc(s, i)) ++j;",
                                           "if (uc(accept, j)) return (char *)(s + i);",
                                           "++i;",
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
                            "void *memcpy(void *d, const void *s, size_t n) {", "void *memmove(void *d, const void *s, size_t n) {",
                            "void *memset(void *d, int c, size_t n) {", "int memcmp(const void *a, const void *b, size_t n) {",
                            "void *memchr(const void *s, int c, size_t n) {", "char *strdup(const char *s) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("strstr contracts: harness calls strstr and strpbrk") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);
    CHECK(cpp.find("int strstr_true(") != std::string::npos);
    CHECK(cpp.find("int strstr_miss_false(") != std::string::npos);
    CHECK(cpp.find("int strpbrk_true(") != std::string::npos);
    CHECK(cpp.find("int strpbrk_miss_false(") != std::string::npos);
    CHECK(cpp.find("assert(strstr(s, empty_n) == s);") != std::string::npos);
    CHECK(cpp.find("assert(strstr(abc, bc) == abc + 1);") != std::string::npos);
    CHECK(cpp.find("assert(strstr(abc, abc) == abc);") != std::string::npos);
    CHECK(cpp.find("assert(strstr(abc, a) == abc);") != std::string::npos);
    CHECK(cpp.find("assert(strstr(s, needle) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strpbrk(s, empty_a) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strpbrk(abc, stop) == abc + 1);") != std::string::npos);
    CHECK(cpp.find("assert(strpbrk(abc, abc) == abc);") != std::string::npos);
    CHECK(cpp.find("assert(strpbrk(abc, z) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strpbrk(s, accept) == 0);") != std::string::npos);
    CHECK(count_of(cpp, "strstr(") >= 1);
    CHECK(count_of(cpp, "strpbrk(") >= 1);
}

TEST_CASE("strstr contracts: yml has a true task, a false task, and FUNC-CONTRACT") {
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: string_contracts.cpp") != std::string::npos);
    CHECK(yml.find("language: C") != std::string::npos);
    CHECK(yml.find("property: libc-contract") != std::string::npos);
    CHECK(yml.find("strstr_true: true") != std::string::npos);
    CHECK(yml.find("strstr_miss_false: false") != std::string::npos);
    CHECK(yml.find("strpbrk_true: true") != std::string::npos);
    CHECK(yml.find("strpbrk_miss_false: false") != std::string::npos);
    CHECK(yml.find("strstr_miss_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(yml.find("strpbrk_miss_false: \"FUNC-CONTRACT\"") != std::string::npos);

    const auto expected = yml.find("expected:");
    const auto classes = yml.find("expect_class:");
    REQUIRE(expected != std::string::npos);
    REQUIRE(classes != std::string::npos);
    CHECK(classes > expected);
    const auto exp = yml.substr(expected, classes - expected);
    const auto cls = yml.substr(classes);
    CHECK(exp.find("strstr_true: true") != std::string::npos);
    CHECK(exp.find("strpbrk_true: true") != std::string::npos);
    CHECK(exp.find("strstr_miss_false: false") != std::string::npos);
    CHECK(exp.find("strpbrk_miss_false: false") != std::string::npos);
    CHECK(cls.find("strstr_miss_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(cls.find("strpbrk_miss_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_strstr.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_strspn.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_string_null.cpp") != std::string::npos);
}
