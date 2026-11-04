// memmem model. Reads the model and the string contract harness as text.
// Does not compile them. docs/finish-notes/memmem.md

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

TEST_CASE("memmem model: zero needle returns haystack, null lengths trap before a read") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 11);
    CHECK(count_of(src, "PROVED") == 11);
    CHECK(count_of(src, "CLEAN") == 11);

    const auto memccpy_at = src.find("void *memccpy(void *d, const void *s, int c, size_t n) {");
    const auto memmem_at = src.find(
        "void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {");
    const auto malloc_at = src.find("void *malloc(size_t n);");
    REQUIRE(memccpy_at != std::string::npos);
    REQUIRE(memmem_at != std::string::npos);
    REQUIRE(malloc_at != std::string::npos);
    CHECK(memccpy_at < memmem_at);
    CHECK(memmem_at < malloc_at);
    CHECK(src.find("void *memmem(", memmem_at + 1) == std::string::npos);

    const auto note_at = src.rfind("/*", memmem_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > memccpy_at);
    const auto note = src.substr(note_at, memmem_at - note_at);
    CHECK(note.find("first match of the needlelen bytes") != std::string::npos);
    CHECK(note.find("A needle length of 0 returns the haystack pointer and does not read,") != std::string::npos);
    CHECK(note.find("even when the haystack pointer is null.") != std::string::npos);
    CHECK(note.find("A null haystack with a haystack length greater than 0 is PTR-NULL-DEREF") != std::string::npos);
    CHECK(note.find("\"memmem null haystack\"") != std::string::npos);
    CHECK(note.find("A null needle with a needle length greater than 0 is PTR-NULL-DEREF") != std::string::npos);
    CHECK(note.find("\"memmem null needle\"") != std::string::npos);
    CHECK(note.find("__builtin_trap before any read.") != std::string::npos);
    CHECK(note.find("reads only inside the two lengths.") != std::string::npos);
    CHECK(note.find("A miss returns a null pointer.") != std::string::npos);
    CHECK(note.find("This is not a proof of the search.") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(count_of(note, "PROVED") == 1);
    CHECK(count_of(note, "CLEAN") == 1);

    const auto body = function_body(
        src, "void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen)");
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);
    CHECK(body ==
          "{\n"
          "    if (needlelen == 0) return (void *)haystack;\n"
          "    if (haystack == 0 && haystacklen > 0) {\n"
          "        __prism_check(0, \"PTR-NULL-DEREF\", \"memmem null haystack\");\n"
          "        __builtin_trap();\n"
          "    }\n"
          "    if (needle == 0 && needlelen > 0) {\n"
          "        __prism_check(0, \"PTR-NULL-DEREF\", \"memmem null needle\");\n"
          "        __builtin_trap();\n"
          "    }\n"
          "    if (needlelen > haystacklen) return 0;\n"
          "    const unsigned char *h = (const unsigned char *)haystack;\n"
          "    const unsigned char *nd = (const unsigned char *)needle;\n"
          "    const size_t last = haystacklen - needlelen;\n"
          "    size_t i = 0;\n"
          "    for (;;) {\n"
          "        size_t j = 0;\n"
          "        while (j < needlelen && h[i + j] == nd[j]) ++j;\n"
          "        if (j == needlelen) return (void *)(h + i);\n"
          "        if (i == last) return 0;\n"
          "        ++i;\n"
          "    }\n"
          "}");

    const auto zero = body.find("if (needlelen == 0) return (void *)haystack;");
    const auto hay = body.find("if (haystack == 0 && haystacklen > 0)");
    const auto hay_check = body.find("__prism_check(0, \"PTR-NULL-DEREF\", \"memmem null haystack\");");
    const auto hay_trap = body.find("__builtin_trap();");
    const auto nd_if = body.find("if (needle == 0 && needlelen > 0)");
    const auto nd_check = body.find("__prism_check(0, \"PTR-NULL-DEREF\", \"memmem null needle\");");
    const auto nd_trap = body.find("__builtin_trap();", hay_trap + 1);
    const auto miss = body.find("if (needlelen > haystacklen) return 0;");
    const auto reads = body.find("const unsigned char *h = (const unsigned char *)haystack;");
    const auto guard = body.find("while (j < needlelen && h[i + j] == nd[j]) ++j;");
    const auto hit = body.find("if (j == needlelen) return (void *)(h + i);");
    const auto stop = body.find("if (i == last) return 0;");
    const auto inc = body.find("++i;");
    REQUIRE(zero != std::string::npos);
    REQUIRE(hay != std::string::npos);
    REQUIRE(hay_check != std::string::npos);
    REQUIRE(hay_trap != std::string::npos);
    REQUIRE(nd_if != std::string::npos);
    REQUIRE(nd_check != std::string::npos);
    REQUIRE(nd_trap != std::string::npos);
    REQUIRE(miss != std::string::npos);
    REQUIRE(reads != std::string::npos);
    REQUIRE(guard != std::string::npos);
    REQUIRE(hit != std::string::npos);
    REQUIRE(stop != std::string::npos);
    REQUIRE(inc != std::string::npos);
    CHECK(zero < hay);
    CHECK(hay < hay_check);
    CHECK(hay_check < hay_trap);
    CHECK(hay_trap < nd_if);
    CHECK(nd_if < nd_check);
    CHECK(nd_check < nd_trap);
    CHECK(nd_trap < miss);
    CHECK(miss < reads);
    CHECK(reads < guard);
    CHECK(guard < hit);
    CHECK(hit < stop);
    CHECK(stop < inc);
    const auto before = body.substr(0, reads);
    CHECK(before.find('[') == std::string::npos);
    CHECK(before.find("uc(") == std::string::npos);
    CHECK(before.find("h[") == std::string::npos);
    CHECK(before.find("nd[") == std::string::npos);
    CHECK(body.find("h[haystacklen]") == std::string::npos);
    CHECK(body.find("nd[needlelen]") == std::string::npos);
    CHECK(body.find("[haystacklen]") == std::string::npos);
    CHECK(body.find("[needlelen]") == std::string::npos);
    CHECK(body.find("h[i + needlelen]") == std::string::npos);
    CHECK(body.find("i + needlelen") == std::string::npos);
    CHECK(count_of(body, "h[") == 1);
    CHECK(count_of(body, "nd[") == 1);
    CHECK(count_of(body, "__builtin_trap();") == 2);

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

    const auto scase = function_body(src, "int strcasecmp(const char *a, const char *b)");
    CHECK(scase ==
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

    const auto ncase = function_body(src, "int strncasecmp(const char *a, const char *b, size_t n)");
    CHECK(ncase ==
          "{\n"
          "    __prism_check(a != 0, \"STR-NULL-ARG\", \"strncasecmp() called with unchecked pointer parameter a\");\n"
          "    if (a == 0) return 0;\n"
          "    __prism_check(b != 0, \"STR-NULL-ARG\", \"strncasecmp() called with unchecked pointer parameter b\");\n"
          "    if (b == 0) return 0;\n"
          "    if (n == 0) return 0;\n"
          "    size_t i = 0;\n"
          "    while (i + 1 < n) {\n"
          "        unsigned char ca = uc(a, i);\n"
          "        unsigned char cb = uc(b, i);\n"
          "        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');\n"
          "        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');\n"
          "        if (!ca || ca != cb) break;\n"
          "        ++i;\n"
          "    }\n"
          "    {\n"
          "        unsigned char ca = uc(a, i);\n"
          "        unsigned char cb = uc(b, i);\n"
          "        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');\n"
          "        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');\n"
          "        if (ca != cb) return ca > cb ? 1 : -1;\n"
          "    }\n"
          "    return 0;\n"
          "}");

    const auto coll = function_body(src, "int strcoll(const char *a, const char *b)");
    CHECK(coll ==
          "{\n"
          "    __prism_check(a != 0, \"STR-NULL-ARG\", \"strcoll() called with unchecked pointer parameter a\");\n"
          "    if (a == 0) return 0;\n"
          "    __prism_check(b != 0, \"STR-NULL-ARG\", \"strcoll() called with unchecked pointer parameter b\");\n"
          "    if (b == 0) return 0;\n"
          "    size_t i = 0;\n"
          "    for (;;) {\n"
          "        const unsigned char ca = uc(a, i);\n"
          "        const unsigned char cb = uc(b, i);\n"
          "        if (ca != cb) return ca > cb ? 1 : -1;\n"
          "        if (!ca) return 0;\n"
          "        ++i;\n"
          "    }\n"
          "}");

    const auto cpy = function_body(src, "void *memccpy(void *d, const void *s, int c, size_t n)");
    CHECK(cpy ==
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
                            "int strcasecmp(const char *a, const char *b) {",
                            "int strncasecmp(const char *a, const char *b, size_t n) {",
                            "int strcoll(const char *a, const char *b) {",
                            "char *strchr(const char *s, int c) {", "char *strrchr(const char *s, int c) {",
                            "size_t strspn(const char *s, const char *accept) {",
                            "size_t strcspn(const char *s, const char *accept) {",
                            "char *strstr(const char *s, const char *needle) {",
                            "char *strpbrk(const char *s, const char *accept) {",
                            "void *memcpy(void *d, const void *s, size_t n) {", "void *memmove(void *d, const void *s, size_t n) {",
                            "void *memset(void *d, int c, size_t n) {",
                            "void *memset_explicit(void *d, int c, size_t n) {",
                            "int memcmp(const void *a, const void *b, size_t n) {",
                            "void *memchr(const void *s, int c, size_t n) {", "void *memrchr(const void *s, int c, size_t n) {",
                            "void *memccpy(void *d, const void *s, int c, size_t n) {",
                            "void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {",
                            "char *strdup(const char *s) {", "char *strndup(const char *s, size_t n) {"}) {
        CAPTURE(sig);
        CHECK(count_of(src, sig) == 1);
    }

    for (const char* rel : {"tests/cpp/test_strcasecmp.cpp", "tests/cpp/test_strncasecmp.cpp",
                            "tests/cpp/test_strcoll.cpp", "tests/cpp/test_memccpy.cpp", "tests/cpp/test_memrchr.cpp",
                            "tests/cpp/test_strndup.cpp", "tests/cpp/test_strspn.cpp", "tests/cpp/test_strstr.cpp"}) {
        CAPTURE(rel);
        const auto text = slurp(repo() / rel);
        REQUIRE(!text.empty());
        CHECK(text.find("CHECK(count_of(src, \"This is not PROVED and not CLEAN.\") == 11);") != std::string::npos);
        CHECK(text.find("CHECK(count_of(src, \"PROVED\") == 11);") != std::string::npos);
        CHECK(text.find("CHECK(count_of(src, \"CLEAN\") == 11);") != std::string::npos);
        CHECK(text.find("CHECK(count_of(src, \"This is not PROVED and not CLEAN.\") == 10);") == std::string::npos);
        CHECK(text.find("CHECK(count_of(src, \"PROVED\") == 10);") == std::string::npos);
        CHECK(text.find("CHECK(count_of(src, \"CLEAN\") == 10);") == std::string::npos);
    }
}

TEST_CASE("memmem contracts: harness calls memmem and yml expects no found offset") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "string_contracts.yml");
    const auto cmake = slurp(repo() / "CMakeLists.txt");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/string.cpp\"") != std::string::npos);

    const auto body = function_body(cpp, "int memchr_true(int c, int n, int j)");
    REQUIRE(!body.empty());
    CHECK(body.find("char *r = (char *)memchr(s, c, (size_t)n);") != std::string::npos);
    CHECK(body.find("assert(i >= 0 && i < n);") != std::string::npos);
    CHECK(body.find("This does not assert a found offset.") != std::string::npos);
    CHECK(body.find("(void)memmem(s, (size_t)n, s, 0);") != std::string::npos);
    CHECK(body.find("assert(memmem") == std::string::npos);
    CHECK(body.find("memmem(s, (size_t)n, s, 0) ==") == std::string::npos);

    const auto bad = function_body(cpp, "int memchr_oob_false(int n)");
    REQUIRE(!bad.empty());
    CHECK(bad.find("memmem") == std::string::npos);

    CHECK(yml.find("memchr_true: true") != std::string::npos);
    CHECK(yml.find("memchr_oob_false: false") != std::string::npos);
    CHECK(yml.find("memmem") == std::string::npos);
    CHECK(count_of(cmake, "tests/cpp/test_memmem.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strcoll.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strncasecmp.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strcasecmp.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_memccpy.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strndup.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_memrchr.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strstr.cpp") == 1);
    CHECK(count_of(cmake, "tests/cpp/test_strspn.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_stdio_buffer.cpp") != std::string::npos);
}
