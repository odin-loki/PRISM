// strtod: a null s returns 0 before strlen. A non-null end stays inside the
// string. The result is a nondet double, not a parsed numeral. Reads the
// model and the libc contract harness as text.

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

}  // namespace

TEST_CASE("strtod model: null s returns 0 before strlen and the value is a nondet double") {
    const auto src = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" /
                           "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());

    const auto atoll_at = src.find("long long atoll(const char *s)");
    const auto strtod_at = src.find("double strtod(const char *s, char **end)");
    const auto strtoul_at = src.find("unsigned long strtoul(const char *s, char **end, int base)");
    REQUIRE(atoll_at != std::string::npos);
    REQUIRE(strtod_at != std::string::npos);
    REQUIRE(strtoul_at != std::string::npos);
    CHECK(atoll_at < strtod_at);
    CHECK(strtod_at < strtoul_at);

    const auto note_at = src.rfind("/*", strtod_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(note_at > atoll_at);
    const auto note = src.substr(note_at, strtod_at - note_at);
    CHECK(note.find("STR-NULL-ARG") != std::string::npos);
    CHECK(note.find("before strlen") != std::string::npos);
    CHECK(note.find("does not call strlen") != std::string::npos);
    CHECK(note.find("does not write *end") != std::string::npos);
    CHECK(note.find("does not scan") != std::string::npos);
    CHECK(note.find("end pointer, when non-null, stays") != std::string::npos);
    CHECK(note.find("inside the string") != std::string::npos);
    CHECK(note.find("nondet double") != std::string::npos);
    CHECK(note.find("not a parsed numeral") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(note.find("double __VERIFIER_nondet_double(void);") != std::string::npos);

    const auto body = function_body(src, "double strtod(const char *s, char **end)");
    REQUIRE(body ==
            "{\n"
            "    __prism_check(s != 0, \"STR-NULL-ARG\", \"strtod() called with unchecked pointer parameter s\");\n"
            "    if (s == 0) return 0;\n"
            "    size_t n = strlen(s);\n"
            "    if (end) {\n"
            "        size_t k = __VERIFIER_nondet_ulong();\n"
            "        __prism_assume(k <= n);\n"
            "        *end = (char *)s + k;\n"
            "    }\n"
            "    return __VERIFIER_nondet_double();\n"
            "}");
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);

    const auto early = body.find("if (s == 0) return 0;");
    const auto scan = body.find("strlen(");
    const auto end_store = body.find("*end = (char *)s + k;");
    REQUIRE(early != std::string::npos);
    REQUIRE(scan != std::string::npos);
    REQUIRE(end_store != std::string::npos);
    CHECK(early < scan);
    CHECK(scan < end_store);
    CHECK(body.substr(0, scan).find("*end") == std::string::npos);
    CHECK(body.find("while") == std::string::npos);
    CHECK(body.find("for (") == std::string::npos);
    CHECK(body.find("s[") == std::string::npos);
    CHECK(body.find("return 1") == std::string::npos);
    CHECK(body.find("return '") == std::string::npos);
    CHECK(body.find("strtol(") == std::string::npos);
    CHECK(body.find("strtoll(") == std::string::npos);
    CHECK(body.find("strtoul(") == std::string::npos);
    CHECK(body.find("atoll(") == std::string::npos);

    const auto atoll = function_body(src, "long long atoll(const char *s)");
    REQUIRE(atoll ==
            "{\n"
            "    __prism_check(s != 0, \"STR-NULL-ARG\", \"atoll() called with unchecked pointer parameter s\");\n"
            "    if (s == 0) return 0;\n"
            "    (void)strlen(s);\n"
            "    return __VERIFIER_nondet_longlong();\n"
            "}");
    const auto toul = function_body(src, "unsigned long strtoul(const char *s, char **end, int base)");
    REQUIRE(toul == "{\n    return (unsigned long)strtol(s, end, base);\n}");
}

TEST_CASE("strtod contracts: harness calls strtod and yml expects no parsed number") {
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
    CHECK(body.find("(void)strtod(") != std::string::npos);
    CHECK(body.find("assert(strtod") == std::string::npos);
    CHECK(body.find("strtod(s) == ") == std::string::npos);
    CHECK(body.find("strtod(\"") == std::string::npos);

    CHECK(yml.find("strtol_true: true") != std::string::npos);
    CHECK(yml.find("strtod") == std::string::npos);

    CHECK(cmake.find("tests/cpp/test_strtod.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_atoll.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_strtoll.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_qsort_null.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_div.cpp") != std::string::npos);
}
