// strcasecmp model. Reads the model and the string contract harness as text.
// Does not compile them. docs/finish-notes/strcasecmp.md

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

TEST_CASE("strcasecmp model: null returns before a read, ASCII fold, stop at difference or NUL") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 9);
    CHECK(count_of(src, "PROVED") == 9);
    CHECK(count_of(src, "CLEAN") == 9);

    const auto strncmp_at = src.find("int strncmp(const char *a, const char *b, size_t n) {");
    const auto strcasecmp_at = src.find("int strcasecmp(const char *a, const char *b) {");
    const auto strchr_at = src.find("char *strchr(const char *s, int c) {");
    REQUIRE(strncmp_at != std::string::npos);
    REQUIRE(strcasecmp_at != std::string::npos);
    REQUIRE(strchr_at != std::string::npos);
    CHECK(strncmp_at < strcasecmp_at);
    CHECK(strcasecmp_at < strchr_at);
    CHECK(src.find("int strcasecmp(", strcasecmp_at + 1) == std::string::npos);

    const auto note_at = src.rfind("/*", strcasecmp_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > strncmp_at);
    const auto note = src.substr(note_at, strcasecmp_at - note_at);
    CHECK(note.find("folding only ASCII A-Z into a-z") != std::string::npos);
    CHECK(note.find("This is not a locale comparison.") != std::string::npos);
    CHECK(note.find("stops at the first difference or the first NUL.") != std::string::npos);
    CHECK(note.find("does not read past that byte.") != std::string::npos);
    CHECK(note.find("A null a or a null b is STR-NULL-ARG and returns 0 before any read.") != std::string::npos);
    CHECK(note.find("This is not a proof of the comparison.") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto body = function_body(src, "int strcasecmp(const char *a, const char *b)");
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);
    CHECK(body.find("locale") == std::string::npos);
    CHECK(body.find("tolower") == std::string::npos);
    CHECK(body ==
          "{\n"
          "    __prism_check(a != 0, \"STR-NULL-ARG\", \"strcasecmp() called with unchecked pointer parameter a\");\n"
          "    if (a == 0) return 0;\n"
          "    __prism_check(b != 0, \"STR-NULL-ARG\", \"strcasecmp() called with unchecked pointer parameter b\");\n"
          "    if (b == 0) return 0;\n"
          "    size_t i = 0;\n"
          "    for (;;) {\n"
          "        unsigned char ca = uc(a, i);\n"
          "        unsigned char cb = uc(b, i);\n"
          "        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');\n"
          "        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');\n"
          "        if (ca != cb) return ca > cb ? 1 : -1;\n"
          "        if (!ca) return 0;\n"
          "        ++i;\n"
          "    }\n"
          "}");

    const auto check_a = body.find(
        "__prism_check(a != 0, \"STR-NULL-ARG\", \"strcasecmp() called with unchecked pointer parameter a\");");
    const auto early_a = body.find("if (a == 0) return 0;");
    const auto check_b = body.find(
        "__prism_check(b != 0, \"STR-NULL-ARG\", \"strcasecmp() called with unchecked pointer parameter b\");");
    const auto early_b = body.find("if (b == 0) return 0;");
    const auto loop = body.find("for (;;)");
    const auto read_a = body.find("unsigned char ca = uc(a, i);");
    const auto read_b = body.find("unsigned char cb = uc(b, i);");
    const auto fold_a = body.find("if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');");
    const auto fold_b = body.find("if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');");
    const auto diff = body.find("if (ca != cb) return ca > cb ? 1 : -1;");
    const auto nul = body.find("if (!ca) return 0;");
    const auto inc = body.find("++i;");
    REQUIRE(check_a != std::string::npos);
    REQUIRE(early_a != std::string::npos);
    REQUIRE(check_b != std::string::npos);
    REQUIRE(early_b != std::string::npos);
    REQUIRE(loop != std::string::npos);
    REQUIRE(read_a != std::string::npos);
    REQUIRE(read_b != std::string::npos);
    REQUIRE(fold_a != std::string::npos);
    REQUIRE(fold_b != std::string::npos);
    REQUIRE(diff != std::string::npos);
    REQUIRE(nul != std::string::npos);
    REQUIRE(inc != std::string::npos);
    CHECK(check_a < early_a);
    CHECK(early_a < check_b);
    CHECK(check_b < early_b);
    CHECK(early_b < loop);
    CHECK(loop < read_a);
    CHECK(read_a < read_b);
    CHECK(read_b < fold_a);
    CHECK(fold_a < fold_b);
    CHECK(fold_b < diff);
    CHECK(diff < nul);
    CHECK(nul < inc);
    const auto before = body.substr(0, loop);
    CHECK(before.find("uc(") == std::string::npos);
    CHECK(before.find('[') == std::string::npos);
    CHECK(body.find("i + 1") == std::string::npos);
    CHECK(body.find("i+1") == std::string::npos);
    CHECK(body.find("[i + 1]") == std::string::npos);
    CHECK(body.find("uc(a, i + 1)") == std::string::npos);
    CHECK(body.find("uc(b, i + 1)") == std::string::npos);
    CHECK(count_of(body, "uc(") == 2);
    CHECK(count_of(body, "++i") == 1);

    const auto cmp = function_body(src, "int strcmp(const char *a, const char *b)");
    CHECK(cmp ==
          "{\n"
          "    __prism_check(a != 0, \"STR-NULL-ARG\", \"strcmp() called with unchecked pointer parameter a\");\n"
          "    __prism_check(b != 0, \"STR-NULL-ARG\", \"strcmp() called with unchecked pointer parameter b\");\n"
          "    size_t i = 0;\n"
          "    while (uc(a, i) && uc(a, i) == uc(b, i)) ++i;\n"
          "    {\n"
          "        const unsigned char ca = uc(a, i);\n"
          "        const unsigned char cb = uc(b, i);\n"
          "        if (ca != cb) return ca > cb ? 1 : -1;\n"
          "    }\n"
          "    return 0;\n"
          "}");

    const auto ncmp = function_body(src, "int strncmp(const char *a, const char *b, size_t n)");
    CHECK(ncmp ==
          "{\n"
          "    __prism_check(a != 0, \"STR-NULL-ARG\", \"strncmp() called with unchecked pointer parameter a\");\n"
          "    __prism_check(b != 0, \"STR-NULL-ARG\", \"strncmp() called with unchecked pointer parameter b\");\n"
          "    if (n == 0) return 0;\n"
          "    size_t i = 0;\n"
          "    while (i + 1 < n) {\n"
          "        const unsigned char ca = uc(a, i);\n"
          "        const unsigned char cb = uc(b, i);\n"
          "        if (!ca || ca != cb) break;\n"
          "        ++i;\n"
          "    }\n"
          "    {\n"
          "        const unsigned char ca = uc(a, i);\n"
          "        const unsigned char cb = uc(b, i);\n"
          "        if (ca != cb) return ca > cb ? 1 : -1;\n"
          "    }\n"
          "    return 0;\n"
          "}");

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
                            "void *memccpy(void *d, const void *s, int c, size_t n) {",
                            "char *strdup(const char *s) {", "char *strndup(const char *s, size_t n) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }
}

TEST_CASE("strcasecmp contracts: harness calls strcasecmp and yml expects no comparison") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);

    const auto body = function_body(cpp, "int strcmp_true(int ka, int kb, int j)");
    REQUIRE(!body.empty());
    CHECK(body.find("int r = strcmp(a, b);") != std::string::npos);
    CHECK(body.find("assert(sign(strcmp(b, a)) == -sign(r));") != std::string::npos);
    CHECK(body.find("(void)strcasecmp(a, b);") != std::string::npos);
    CHECK(body.find("assert(strcasecmp") == std::string::npos);
    CHECK(body.find("strcasecmp(a, b) ==") == std::string::npos);

    const auto bad = function_body(cpp, "int strcmp_signed_false(void)");
    REQUIRE(!bad.empty());
    CHECK(bad.find("strcasecmp") == std::string::npos);

    CHECK(yml.find("strcmp_true: true") != std::string::npos);
    CHECK(yml.find("strcmp_signed_false: false") != std::string::npos);
    CHECK(yml.find("strcasecmp") == std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_strcasecmp.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_memccpy.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strndup.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_memrchr.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strstr.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strspn.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_stdio_buffer.cpp") != std::string::npos);
}
