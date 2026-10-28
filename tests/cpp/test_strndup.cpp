// strndup model. Reads the model and the string contract harness as text.
// Does not compile them. docs/finish-notes/strndup.md

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

TEST_CASE("strndup model: null returns before allocation, n == 0 does not read s") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 6);
    CHECK(count_of(src, "PROVED") == 6);
    CHECK(count_of(src, "CLEAN") == 6);

    const auto strdup_at = src.find("char *strdup(const char *s) {");
    const auto strndup_at = src.find("char *strndup(const char *s, size_t n) {");
    const auto end_c = src.find("}  // extern \"C\"", strndup_at);
    REQUIRE(strdup_at != std::string::npos);
    REQUIRE(strndup_at != std::string::npos);
    REQUIRE(end_c != std::string::npos);
    CHECK(strdup_at < strndup_at);
    CHECK(strndup_at < end_c);
    CHECK(src.find("char *strndup(", strndup_at + 1) == std::string::npos);

    const auto note_at = src.rfind("/*", strndup_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > strdup_at);
    const auto note = src.substr(note_at, strndup_at - note_at);
    CHECK(note.find("before strlen") != std::string::npos);
    CHECK(note.find("before any allocation") != std::string::npos);
    CHECK(note.find("n == 0 does not read s") != std::string::npos);
    CHECK(note.find("allocates one byte") != std::string::npos);
    CHECK(note.find("min(n, strlen(s))") != std::string::npos);
    CHECK(note.find("does not read s[i] for i >= n") != std::string::npos);
    CHECK(note.find("does not read past the terminator") != std::string::npos);
    CHECK(note.find("SIZE_MAX has no room for the trailing") != std::string::npos);
    CHECK(note.find("does not call malloc") != std::string::npos);
    CHECK(note.find("This is not a proof of the copy.") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto body = function_body(src, "char *strndup(const char *s, size_t n)");
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);
    CHECK(body.find("strlen(") == std::string::npos);
    CHECK(body.find("__prism_memcpy") == std::string::npos);

    const auto check = body.find(
        "__prism_check(s != 0, \"STR-NULL-ARG\", \"strndup() called with unchecked pointer parameter s\");");
    const auto early = body.find("if (s == 0) return 0;");
    const auto zero = body.find("if (n == 0) {");
    REQUIRE(check != std::string::npos);
    REQUIRE(early != std::string::npos);
    REQUIRE(zero != std::string::npos);
    CHECK(check < early);
    CHECK(early < zero);
    const auto before_alloc = body.substr(0, zero);
    CHECK(before_alloc.find("malloc") == std::string::npos);
    CHECK(before_alloc.find("s[") == std::string::npos);
    CHECK(before_alloc.find("strlen") == std::string::npos);

    const auto zero_end = body.find("return p;", zero);
    REQUIRE(zero_end != std::string::npos);
    const auto zero_arm = body.substr(zero, zero_end - zero);
    CHECK(zero_arm.find("malloc(1)") != std::string::npos);
    CHECK(zero_arm.find("if (!p) return 0;") != std::string::npos);
    CHECK(zero_arm.find("p[0] = 0;") != std::string::npos);
    CHECK(zero_arm.find("s[") == std::string::npos);
    CHECK(zero_arm.find("strlen") == std::string::npos);

    const auto scan = body.find("while (len < n && s[len]) ++len;");
    const auto alloc = body.find("char *p = (char *)malloc(len + 1);", zero_end);
    const auto fail = body.find("if (!p) return 0;", alloc);
    const auto copy = body.find("for (size_t i = 0; i < len; ++i) p[i] = s[i];");
    const auto term = body.find("p[len] = 0;");
    REQUIRE(scan != std::string::npos);
    REQUIRE(alloc != std::string::npos);
    REQUIRE(fail != std::string::npos);
    REQUIRE(copy != std::string::npos);
    REQUIRE(term != std::string::npos);
    CHECK(zero_end < scan);
    CHECK(scan < alloc);
    const auto fit = body.find(
        "__prism_check(0, \"MEM-OOB-WRITE\", \"strndup length does not fit in size_t\");", scan);
    REQUIRE(fit != std::string::npos);
    const auto trap = body.find("__builtin_trap();", fit);
    REQUIRE(trap != std::string::npos);
    CHECK(scan < fit);
    CHECK(fit < trap);
    CHECK(trap < alloc);
    CHECK(alloc < fail);
    CHECK(fail < copy);
    CHECK(copy < term);
    CHECK(body.find("s[n]") == std::string::npos);
    CHECK(body.find("s[i + 1]") == std::string::npos);
    CHECK(body.find("s[len + 1]") == std::string::npos);

    const auto dup = function_body(src, "char *strdup(const char *s)");
    CHECK(dup ==
          "{\n"
          "    __prism_check(s != 0, \"STR-NULL-ARG\", \"strdup() called with unchecked pointer parameter s\");\n"
          "    size_t n = strlen(s);\n"
          "    char *p = (char *)malloc(n + 1);\n"
          "    if (!p) return 0;\n"
          "    __prism_memcpy(p, s, n + 1, 0);\n"
          "    return p;\n"
          "}");

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
                            "char *strdup(const char *s) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("strndup contracts: harness calls strndup and yml expects no string") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);

    const auto body = function_body(cpp, "int strdup_true(int k, int j)");
    REQUIRE(!body.empty());
    CHECK(body.find("char *p = strdup(s);") != std::string::npos);
    CHECK(body.find("char *q = strndup(s, (size_t)k);") != std::string::npos);
    CHECK(body.find("if (q) free(q);") != std::string::npos);
    CHECK(body.find("assert(q") == std::string::npos);
    CHECK(body.find("strndup(s) == ") == std::string::npos);
    CHECK(body.find("strcmp(") == std::string::npos);

    CHECK(yml.find("strdup_true: true") != std::string::npos);
    CHECK(yml.find("strndup") == std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_strndup.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_memrchr.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_qsort_null.cpp") != std::string::npos);
}
