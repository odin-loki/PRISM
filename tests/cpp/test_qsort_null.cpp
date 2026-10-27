// qsort and bsearch models: a zero count does not read, a null pointer is a
// check, and a wrapping length is not walked. Reads stdlib.cpp as text.

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
    // Checkout may be CRLF. The locked texts are compared with '\n' only.
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

std::string brace_block_after(std::string_view src, std::size_t at) {
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i + 1 - open));
        }
    }
    return {};
}

std::string check_message(std::string_view src, std::size_t check) {
    int quotes = 0;
    std::size_t start = 0;
    for (std::size_t i = check; i < src.size(); ++i) {
        if (src[i] != '"') continue;
        ++quotes;
        if (quotes == 3) start = i + 1;
        if (quotes == 4) return std::string(src.substr(start, i - start));
    }
    return {};
}

bool has_read(std::string_view s) {
    return s.find("base[") != std::string_view::npos || s.find("*base") != std::string_view::npos ||
           s.find("key[") != std::string_view::npos || s.find("*key") != std::string_view::npos ||
           s.find("compar(") != std::string_view::npos || s.find("(*compar)") != std::string_view::npos ||
           s.find("__prism_read_range") != std::string_view::npos ||
           s.find("__prism_memcpy") != std::string_view::npos ||
           s.find("__prism_havoc") != std::string_view::npos ||
           s.find("__prism_memset") != std::string_view::npos || s.find("for") != std::string_view::npos ||
           s.find("while") != std::string_view::npos || s.find("< nmemb") != std::string_view::npos ||
           s.find("<= nmemb") != std::string_view::npos || s.find("< nbytes") != std::string_view::npos;
}

}  // namespace

TEST_CASE("qsort model: zero count returns before a read or a compar call") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "void qsort(");
    REQUIRE(!body.empty());
    const auto check = body.find("__prism_check");
    REQUIRE(check != std::string::npos);
    const auto prefix = body.substr(0, check);
    CHECK(prefix.find("nmemb == 0 || size == 0") != std::string::npos);
    CHECK(prefix.find("return") != std::string::npos);
    CHECK_FALSE(has_read(prefix));
    CHECK(prefix.find("compar(") == std::string::npos);
}

TEST_CASE("qsort model: null base and null compar are checks and are not followed by a read") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "void qsort(");
    REQUIRE(!body.empty());
    const auto el = body.find("else");
    REQUIRE(el != std::string::npos);
    const auto rest = brace_block_after(body, el);
    REQUIRE(!rest.empty());
    const auto base_check = rest.find("__prism_check(");
    REQUIRE(base_check != std::string::npos);
    CHECK(rest.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(check_message(rest, base_check).find("qsort()") != std::string::npos);
    const auto base_ret = rest.find("if (base == 0) return;", base_check);
    REQUIRE(base_ret != std::string::npos);
    CHECK_FALSE(has_read(rest.substr(base_check, base_ret - base_check)));

    const auto compar_check = rest.find("__prism_check(", base_ret);
    REQUIRE(compar_check != std::string::npos);
    CHECK(rest.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(check_message(rest, compar_check).find("qsort()") != std::string::npos);
    const auto compar_ret = rest.find("if (compar == 0) return;", compar_check);
    REQUIRE(compar_ret != std::string::npos);
    CHECK(compar_check < compar_ret);
    CHECK_FALSE(has_read(rest.substr(compar_ret)));
    CHECK(rest.find("base[") == std::string::npos);
    CHECK(rest.find("compar(") == std::string::npos);
}

TEST_CASE("qsort model: wrapping nmemb * size is a check and does not sort") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    const auto qsort_at = src.find("void qsort(");
    const auto note_at = src.rfind("/* qsort", qsort_at);
    REQUIRE(qsort_at != std::string::npos);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, qsort_at - note_at);
    CHECK(note.find("does not claim the array is sorted") != std::string::npos);
    CHECK(note.find("qsort is not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "void qsort(");
    REQUIRE(!body.empty());
    const auto mul = body.find("__builtin_mul_overflow");
    REQUIRE(mul != std::string::npos);
    const auto arm = brace_block_after(body, mul);
    REQUIRE(!arm.empty());
    CHECK(arm.find("__prism_check(") != std::string::npos);
    CHECK(arm.find("MEM-OOB-READ") != std::string::npos);
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return") == std::string::npos);
    CHECK_FALSE(has_read(arm));
    CHECK(check_message(arm, arm.find("__prism_check(")).find("qsort") != std::string::npos);

    const auto el = body.find("else");
    const auto rest = brace_block_after(body, el);
    CHECK(rest.find("The bytes stay as they were") != std::string::npos);
    CHECK(rest.find("No order is invented") != std::string::npos);
    CHECK(rest.find("__prism_memcpy") == std::string::npos);
    CHECK_FALSE(has_read(body));
}

TEST_CASE("bsearch model: zero count and null pointers return NULL without a read") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("void *bsearch(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/* A miss returns NULL", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("not read past nmemb") != std::string::npos);

    const auto body = function_body(src, "void *bsearch(");
    REQUIRE(!body.empty());
    const auto check = body.find("__prism_check");
    REQUIRE(check != std::string::npos);
    const auto prefix = body.substr(0, check);
    CHECK(prefix.find("nmemb == 0 || size == 0") != std::string::npos);
    CHECK(prefix.find("return 0") != std::string::npos);
    CHECK_FALSE(has_read(prefix));

    const auto mul = body.find("__builtin_mul_overflow");
    REQUIRE(mul != std::string::npos);
    const auto arm = brace_block_after(body, mul);
    REQUIRE(!arm.empty());
    const auto arm_check = arm.find("__prism_check(");
    REQUIRE(arm_check != std::string::npos);
    CHECK(arm.find("MEM-OOB-READ") != std::string::npos);
    CHECK(arm.find("return 0") != std::string::npos);
    CHECK(arm_check < arm.find("return 0"));
    CHECK_FALSE(has_read(arm));
    CHECK(arm.find("for") == std::string::npos);
    CHECK(arm.find("while") == std::string::npos);

    const auto key_check = body.find("__prism_check(key != 0");
    const auto base_check = body.find("__prism_check(base != 0");
    const auto compar_check = body.find("__prism_check(compar != 0");
    REQUIRE(key_check != std::string::npos);
    REQUIRE(base_check != std::string::npos);
    REQUIRE(compar_check != std::string::npos);
    CHECK(key_check < base_check);
    CHECK(base_check < compar_check);
    CHECK(body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(check_message(body, key_check).find("bsearch()") != std::string::npos);
    CHECK(check_message(body, base_check).find("bsearch()") != std::string::npos);
    CHECK(check_message(body, compar_check).find("bsearch()") != std::string::npos);
    CHECK(body.find("if (key == 0) return 0;") != std::string::npos);
    CHECK(body.find("if (base == 0) return 0;") != std::string::npos);
    CHECK(body.find("if (compar == 0) return 0;") != std::string::npos);

    std::size_t returns = 0;
    for (std::size_t i = 0;;) {
        const auto r = body.find("return", i);
        if (r == std::string::npos) break;
        CHECK(body.substr(r, 9) == "return 0;");
        ++returns;
        i = r + 6;
    }
    CHECK(returns >= 4);
    CHECK(body.find("return base") == std::string::npos);
    CHECK(body.find("return key") == std::string::npos);
    CHECK(body.find("__prism_alloc") == std::string::npos);
    CHECK_FALSE(has_read(body));
}

TEST_CASE("stdlib model: qsort leaves rand and the other bodies unchanged") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    const char* kept[] = {
        "void *malloc(size_t n) {\n"
        "    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) {\n"
        "        __prism_alloc_failed();\n"
        "        return 0;\n"
        "    }\n"
        "    return __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);\n"
        "}",
        "void *calloc(size_t n, size_t sz) {\n"
        "    size_t t;\n"
        "    if (__builtin_mul_overflow(n, sz, &t) || t >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) {\n"
        "        __prism_alloc_failed();\n"
        "        return 0;\n"
        "    }\n"
        "    return __prism_alloc(t, PRISM_HEAP, PRISM_ZERO);\n"
        "}",
        "void free(void *p) { __prism_free(p, PRISM_HEAP); }",
        "void *realloc(void *p, size_t n) {\n"
        "    if (!p) return malloc(n);\n"
        "    __prism_free_check(p, PRISM_HEAP);\n"
        "    if (n == 0) {\n"
        "        __prism_free(p, PRISM_HEAP);\n"
        "        return 0;\n"
        "    }\n"
        "    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) { /* p stays valid */\n"
        "        __prism_alloc_failed();\n"
        "        return 0;\n"
        "    }\n"
        "    size_t old = __prism_obj_size(p);\n"
        "    void *q = __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);\n"
        "    __prism_memcpy(q, p, old < n ? old : n, 0);\n"
        "    __prism_free(p, PRISM_HEAP);\n"
        "    return q;\n"
        "}",
        "int abs(int x) { return x < 0 ? -x : x; }",
        "long labs(long x) { return x < 0 ? -x : x; }",
        "long long llabs(long long x) { return x < 0 ? -x : x; }",
        "int atoi(const char *s) {\n"
        "    __prism_check(s != 0, \"STR-NULL-ARG\", \"atoi() called with unchecked pointer parameter s\");\n"
        "    (void)strlen(s);\n"
        "    return __VERIFIER_nondet_int();\n"
        "}",
        "long atol(const char *s) {\n"
        "    __prism_check(s != 0, \"STR-NULL-ARG\", \"atol() called with unchecked pointer parameter s\");\n"
        "    (void)strlen(s);\n"
        "    return __VERIFIER_nondet_long();\n"
        "}",
        "long strtol(const char *s, char **end, int base) {\n"
        "    __prism_check(s != 0, \"STR-NULL-ARG\", \"strtol() called with unchecked pointer parameter s\");\n"
        "    size_t n = strlen(s);\n"
        "    (void)base;\n"
        "    if (end) {\n"
        "        size_t k = __VERIFIER_nondet_ulong();\n"
        "        __prism_assume(k <= n);\n"
        "        *end = (char *)s + k;\n"
        "    }\n"
        "    return __VERIFIER_nondet_long();\n"
        "}",
        "unsigned long strtoul(const char *s, char **end, int base) {\n"
        "    return (unsigned long)strtol(s, end, base);\n"
        "}",
        "/* srand and rand: the seed is not stored.\n"
        " * rand is not a reproducible sequence.\n"
        " * A run of rand is not PROVED and not CLEAN. */\n"
        "int rand(void) {\n"
        "    int r = __VERIFIER_nondet_int();\n"
        "    __prism_assume(r >= 0);\n"
        "    return r;\n"
        "}\n"
        "void srand(unsigned s) { (void)s; }",
        "char *getenv(const char *name) {\n"
        "    __prism_check(name != 0, \"STR-NULL-ARG\", \"getenv() called with unchecked pointer parameter name\");\n"
        "    (void)strlen(name);\n"
        "    if (__VERIFIER_nondet_int()) return 0;\n"
        "    return __prism_fresh_cstr(PRISM_CONST);\n"
        "}",
        "void *operator new(size_t n) {\n"
        "    __prism_assume(n < PRISM_MAX_OBJ);\n"
        "    return __prism_alloc(n, PRISM_NEW, PRISM_UNINIT);\n"
        "}",
        "void *operator new[](size_t n) {\n"
        "    __prism_assume(n < PRISM_MAX_OBJ);\n"
        "    return __prism_alloc(n, PRISM_NEWARR, PRISM_UNINIT);\n"
        "}",
        "void operator delete(void *p) noexcept { __prism_free(p, PRISM_NEW); }",
        "void operator delete(void *p, size_t n) noexcept {\n"
        "    (void)n;\n"
        "    __prism_free(p, PRISM_NEW);\n"
        "}",
        "void operator delete[](void *p) noexcept { __prism_free(p, PRISM_NEWARR); }",
        "void operator delete[](void *p, size_t n) noexcept {\n"
        "    (void)n;\n"
        "    __prism_free(p, PRISM_NEWARR);\n"
        "}",
    };
    for (const char* snippet : kept) CHECK(src.find(snippet) != std::string::npos);

    const auto rand_at = src.find("int rand(void)");
    const auto qsort_at = src.find("void qsort(");
    const auto bsearch_at = src.find("void *bsearch(");
    const auto getenv_at = src.find("char *getenv(");
    const auto end_c = src.find("}  // extern \"C\"");
    REQUIRE(rand_at != std::string::npos);
    REQUIRE(getenv_at != std::string::npos);
    REQUIRE(qsort_at != std::string::npos);
    REQUIRE(bsearch_at != std::string::npos);
    REQUIRE(end_c != std::string::npos);
    CHECK(rand_at < getenv_at);
    CHECK(getenv_at < qsort_at);
    CHECK(qsort_at < bsearch_at);
    CHECK(bsearch_at < end_c);
    CHECK(src.find("void qsort(") == qsort_at);
}
