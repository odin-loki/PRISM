// strspn and strcspn models. Reads the model and the string contract harness
// as text. Does not compile them. docs/finish-notes/strspn.md

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

TEST_CASE("strspn model: span of accept, empty inputs, null returns 0 without a scan") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 4);
    CHECK(count_of(src, "PROVED") == 4);
    CHECK(count_of(src, "CLEAN") == 4);

    const auto strrchr_at = src.find("char *strrchr(");
    const auto strspn_at = src.find("size_t strspn(");
    const auto strcspn_at = src.find("size_t strcspn(");
    const auto memcpy_at = src.find("void *memcpy(");
    REQUIRE(strrchr_at != std::string::npos);
    REQUIRE(strspn_at != std::string::npos);
    REQUIRE(strcspn_at != std::string::npos);
    REQUIRE(memcpy_at != std::string::npos);
    CHECK(strrchr_at < strspn_at);
    CHECK(strspn_at < strcspn_at);
    CHECK(strcspn_at < memcpy_at);
    CHECK(src.find("size_t strspn(", strspn_at + 1) == std::string::npos);
    CHECK(src.find("size_t strcspn(", strcspn_at + 1) == std::string::npos);

    const auto spn_note = src.substr(strrchr_at, strspn_at - strrchr_at);
    CHECK(spn_note.find("how many leading characters of s are in accept") != std::string::npos);
    CHECK(spn_note.find("does not read past the terminator") != std::string::npos);
    CHECK(spn_note.find("An empty s returns 0.") != std::string::npos);
    CHECK(spn_note.find("An empty accept returns 0.") != std::string::npos);
    CHECK(spn_note.find("without a scan") != std::string::npos);
    CHECK(spn_note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto cspn_note = src.substr(strspn_at, strcspn_at - strspn_at);
    CHECK(cspn_note.find("how many leading characters of s are not in accept") != std::string::npos);
    CHECK(cspn_note.find("empty accept returns the length of s") != std::string::npos);
    CHECK(cspn_note.find("does not read past s") != std::string::npos);
    CHECK(cspn_note.find("The first character that is in accept stops the scan.") != std::string::npos);
    CHECK(cspn_note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const std::string spn_sig = "size_t strspn(const char *s, const char *accept) {";
    const std::string cspn_sig = "size_t strcspn(const char *s, const char *accept) {";
    CHECK(statements(src, spn_sig) == std::vector<std::string>{
                                          "__prism_check(s != 0, \"STR-NULL-ARG\", \"strspn() called with unchecked pointer parameter s\");",
                                          "if (s == 0) return 0;",
                                          "__prism_check(accept != 0, \"STR-NULL-ARG\", \"strspn() called with unchecked pointer parameter accept\");",
                                          "if (accept == 0) return 0;",
                                          "if (!uc(accept, 0)) return 0;",
                                          "if (!uc(s, 0)) return 0;",
                                          "size_t n = 0;",
                                          "while (uc(s, n)) {",
                                          "size_t j = 0;",
                                          "while (uc(accept, j) != 0 && uc(accept, j) != uc(s, n)) ++j;",
                                          "if (!uc(accept, j)) return n;",
                                          "++n;",
                                          "}",
                                          "return n;",
                                      });
    CHECK(statements(src, cspn_sig) == std::vector<std::string>{
                                           "__prism_check(s != 0, \"STR-NULL-ARG\", \"strcspn() called with unchecked pointer parameter s\");",
                                           "if (s == 0) return 0;",
                                           "__prism_check(accept != 0, \"STR-NULL-ARG\", \"strcspn() called with unchecked pointer parameter accept\");",
                                           "if (accept == 0) return 0;",
                                           "if (!uc(accept, 0)) {",
                                           "size_t n = 0;",
                                           "while (uc(s, n)) ++n;",
                                           "return n;",
                                           "}",
                                           "if (!uc(s, 0)) return 0;",
                                           "size_t n = 0;",
                                           "while (uc(s, n)) {",
                                           "size_t j = 0;",
                                           "while (uc(accept, j) != 0 && uc(accept, j) != uc(s, n)) ++j;",
                                           "if (uc(accept, j)) return n;",
                                           "++n;",
                                           "}",
                                           "return n;",
                                       });

    for (const char* sig : {"size_t strlen(const char *s) {", "size_t strnlen(const char *s, size_t n) {",
                            "char *strcpy(char *d, const char *s) {", "char *strncpy(char *d, const char *s, size_t n) {",
                            "char *strcat(char *d, const char *s) {", "char *strncat(char *d, const char *s, size_t n) {",
                            "int strcmp(const char *a, const char *b) {", "int strncmp(const char *a, const char *b, size_t n) {",
                            "char *strchr(const char *s, int c) {", "char *strrchr(const char *s, int c) {",
                            "void *memcpy(void *d, const void *s, size_t n) {", "void *memmove(void *d, const void *s, size_t n) {",
                            "void *memset(void *d, int c, size_t n) {", "int memcmp(const void *a, const void *b, size_t n) {",
                            "void *memchr(const void *s, int c, size_t n) {", "char *strdup(const char *s) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("strspn contracts: harness calls strspn and strcspn") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);
    CHECK(cpp.find("int strspn_true(") != std::string::npos);
    CHECK(cpp.find("int strspn_absent_false(") != std::string::npos);
    CHECK(cpp.find("int strcspn_true(") != std::string::npos);
    CHECK(cpp.find("int strcspn_stop_false(") != std::string::npos);
    CHECK(cpp.find("assert(strspn(s, empty_a) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strspn(empty_s, s) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strspn(abc, ab) == 2);") != std::string::npos);
    CHECK(cpp.find("assert(strspn(abc, abc) == 3);") != std::string::npos);
    CHECK(cpp.find("assert(strspn(s, accept) == 3);") != std::string::npos);
    CHECK(cpp.find("assert(strcspn(s, empty_a) == (size_t)k);") != std::string::npos);
    CHECK(cpp.find("assert(strcspn(empty_s, s) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strcspn(abc, stop) == 1);") != std::string::npos);
    CHECK(cpp.find("assert(strcspn(abc, abc) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(strcspn(s, accept) == 3);") != std::string::npos);
    CHECK(count_of(cpp, "strspn(") >= 1);
    CHECK(count_of(cpp, "strcspn(") >= 1);
}

TEST_CASE("strspn contracts: yml has a true task, a false task, and FUNC-CONTRACT") {
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: string_contracts.cpp") != std::string::npos);
    CHECK(yml.find("language: C") != std::string::npos);
    CHECK(yml.find("property: libc-contract") != std::string::npos);
    CHECK(yml.find("strspn_true: true") != std::string::npos);
    CHECK(yml.find("strspn_absent_false: false") != std::string::npos);
    CHECK(yml.find("strcspn_true: true") != std::string::npos);
    CHECK(yml.find("strcspn_stop_false: false") != std::string::npos);
    CHECK(yml.find("strspn_absent_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(yml.find("strcspn_stop_false: \"FUNC-CONTRACT\"") != std::string::npos);

    const auto expected = yml.find("expected:");
    const auto classes = yml.find("expect_class:");
    REQUIRE(expected != std::string::npos);
    REQUIRE(classes != std::string::npos);
    CHECK(classes > expected);
    const auto exp = yml.substr(expected, classes - expected);
    const auto cls = yml.substr(classes);
    CHECK(exp.find("strspn_true: true") != std::string::npos);
    CHECK(exp.find("strcspn_true: true") != std::string::npos);
    CHECK(exp.find("strspn_absent_false: false") != std::string::npos);
    CHECK(exp.find("strcspn_stop_false: false") != std::string::npos);
    CHECK(cls.find("strspn_absent_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(cls.find("strcspn_stop_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_strspn.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_string_null.cpp") != std::string::npos);
}
