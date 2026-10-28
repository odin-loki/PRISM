// strtoll and strtoull: a null s returns 0 before strlen. strtoull does not
// call strtol. Reads the model and the libc contract harness as text.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string slurp(const std::filesystem::path& p) {
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

void check_null_before_strlen(std::string_view src, std::string_view sig, std::string_view message,
                              std::string_view value_return) {
    const auto body = function_body(src, sig);
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string_view::npos);
    CHECK(body.find("CLEAN") == std::string_view::npos);

    const auto check = body.find("__prism_check(s != 0, \"STR-NULL-ARG\", \"");
    const auto early = body.find("if (s == 0) return 0;");
    const auto scan = body.find("strlen(");
    REQUIRE(check != std::string_view::npos);
    REQUIRE(early != std::string_view::npos);
    REQUIRE(scan != std::string_view::npos);
    CHECK(check < early);
    CHECK(early < scan);
    CHECK(body.find(message) != std::string_view::npos);

    const auto before_strlen = body.substr(0, scan);
    CHECK(before_strlen.find("*end") == std::string::npos);
    CHECK(before_strlen.find("strlen") == std::string::npos);
    CHECK(body.substr(early, scan - early).find("*end") == std::string::npos);

    CHECK(body.find("while") == std::string_view::npos);
    CHECK(body.find("for (") == std::string_view::npos);
    CHECK(body.find("s[") == std::string_view::npos);

    const auto end_store = body.find("*end = (char *)s + k;");
    const auto assume = body.find("__prism_assume(k <= n);");
    const auto offset = body.find("size_t k = __VERIFIER_nondet_ulong();");
    REQUIRE(end_store != std::string_view::npos);
    REQUIRE(assume != std::string_view::npos);
    REQUIRE(offset != std::string_view::npos);
    CHECK(scan < offset);
    CHECK(offset < assume);
    CHECK(assume < end_store);

    const auto value = body.find(value_return);
    REQUIRE(value != std::string_view::npos);
    CHECK(end_store < value);
    CHECK(body.find("return 1") == std::string_view::npos);
    CHECK(body.find("return '") == std::string_view::npos);
}

}  // namespace

TEST_CASE("strtoll model: null s returns 0 before strlen and strtoull does not call strtol") {
    const auto src = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" /
                           "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());

    const auto strtol_at = src.find("long strtol(const char *s, char **end, int base)");
    const auto strtoll_at = src.find("long long strtoll(const char *s, char **end, int base)");
    const auto strtoull_at = src.find("unsigned long long strtoull(const char *s, char **end, int base)");
    const auto strtoul_at = src.find("unsigned long strtoul(const char *s, char **end, int base)");
    REQUIRE(strtol_at != std::string::npos);
    REQUIRE(strtoll_at != std::string::npos);
    REQUIRE(strtoull_at != std::string::npos);
    REQUIRE(strtoul_at != std::string::npos);
    CHECK(strtol_at < strtoll_at);
    CHECK(strtoll_at < strtoull_at);
    CHECK(strtoull_at < strtoul_at);

    const auto note_at = src.rfind("/*", strtoll_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > strtol_at);
    const auto note = src.substr(note_at, strtoll_at - note_at);
    CHECK(note.find("STR-NULL-ARG") != std::string::npos);
    CHECK(note.find("does not call strlen") != std::string::npos);
    CHECK(note.find("does not write *end") != std::string::npos);
    CHECK(note.find("does not scan") != std::string::npos);
    CHECK(note.find("not a conversion proof") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("not a cast of strtol or strtoll") != std::string::npos);
    CHECK(note.find("long long __VERIFIER_nondet_longlong(void);") != std::string::npos);
    CHECK(note.find("unsigned long long __VERIFIER_nondet_ulonglong(void);") != std::string::npos);

    check_null_before_strlen(src, "long long strtoll(const char *s, char **end, int base)",
                             "strtoll() called with unchecked pointer parameter s",
                             "return __VERIFIER_nondet_longlong();");
    check_null_before_strlen(src, "unsigned long long strtoull(const char *s, char **end, int base)",
                             "strtoull() called with unchecked pointer parameter s",
                             "return __VERIFIER_nondet_ulonglong();");

    const auto ull = function_body(src, "unsigned long long strtoull(const char *s, char **end, int base)");
    REQUIRE(!ull.empty());
    CHECK(ull.find("strtol(") == std::string::npos);
    CHECK(ull.find("strtoll(") == std::string::npos);
    CHECK(ull.find("__VERIFIER_nondet_long(") == std::string::npos);
    CHECK(ull.find("__VERIFIER_nondet_longlong(") == std::string::npos);
    CHECK(ull.find("(unsigned long)strtol") == std::string::npos);
    CHECK(ull.find("(unsigned long long)strtol") == std::string::npos);
    CHECK(ull.find("(unsigned long long)strtoll") == std::string::npos);
    CHECK(ull.find("(long long)") == std::string::npos);

    const auto tol = function_body(src, "long strtol(const char *s, char **end, int base)");
    REQUIRE(!tol.empty());
    CHECK(tol.find("if (s == 0) return 0;") == std::string::npos);
    CHECK(tol.find("return __VERIFIER_nondet_long();") != std::string::npos);
    const auto toul = function_body(src, "unsigned long strtoul(const char *s, char **end, int base)");
    REQUIRE(toul == "{\n    return (unsigned long)strtol(s, end, base);\n}");
}

TEST_CASE("strtoll contracts: harness calls strtoll and strtoull and yml expects no parsed number") {
    const auto cpp = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "libc-models" /
                           "stdlib_contracts.cpp");
    const auto yml = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "libc-models" /
                           "stdlib_contracts.yml");
    const auto cmake = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "CMakeLists.txt");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    REQUIRE(!cmake.empty());

    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/stdlib.cpp\"") != std::string::npos);
    const auto body = function_body(cpp, "int strtol_true(int k)");
    REQUIRE(!body.empty());
    CHECK(body.find("(void)strtol(") != std::string::npos);
    CHECK(body.find("(void)strtoll(") != std::string::npos);
    CHECK(body.find("(void)strtoull(") != std::string::npos);
    CHECK(body.find("assert(strtoll") == std::string::npos);
    CHECK(body.find("assert(strtoull") == std::string::npos);
    CHECK(body.find("strtoll(s) == ") == std::string::npos);
    CHECK(body.find("strtoull(s) == ") == std::string::npos);

    CHECK(yml.find("strtol_true: true") != std::string::npos);
    CHECK(yml.find("strtoll") == std::string::npos);
    CHECK(yml.find("strtoull") == std::string::npos);

    CHECK(cmake.find("tests/cpp/test_strtoll.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_qsort_null.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_div.cpp") != std::string::npos);
}
