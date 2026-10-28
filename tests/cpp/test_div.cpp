// div, ldiv, and lldiv: division by zero and the unrepresentable minimum
// are a contract check then a trap. Other values use toward-zero division.
// Reads the model and the libc contract harness as text.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
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

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
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

std::string strip_hash_comment(std::string s) {
    bool quote = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quote = !quote;
        if (!quote && s[i] == '#' && (i == 0 || s[i - 1] == ' ')) return trim(s.substr(0, i));
    }
    return trim(s);
}

void check_div(std::string_view src, std::string_view sig, std::string_view min_name, std::string_view result_type,
               std::string_view zero_msg, std::string_view min_msg) {
    const auto body = function_body(src, sig);
    REQUIRE(!body.empty());
    CHECK(body.find("PROVED") == std::string::npos);
    CHECK(body.find("CLEAN") == std::string::npos);
    CHECK(body.find("%") == std::string::npos);
    CHECK(body.find("-dividend") == std::string::npos);
    CHECK(body.find("unsigned") == std::string::npos);
    CHECK(body.find("__VERIFIER_nondet") == std::string::npos);

    const auto zero_if = body.find("if (divisor == 0)");
    REQUIRE(zero_if != std::string::npos);
    const auto zero_arm = brace_block_after(body, zero_if);
    REQUIRE(!zero_arm.empty());
    const auto zero_check = zero_arm.find("__prism_check(0, \"FUNC-CONTRACT\", \"");
    const auto zero_trap = zero_arm.find("__builtin_trap()");
    REQUIRE(zero_check != std::string::npos);
    REQUIRE(zero_trap != std::string::npos);
    CHECK(zero_check < zero_trap);
    CHECK(zero_arm.find(zero_msg) != std::string::npos);
    CHECK(zero_arm.find("return") == std::string::npos);
    CHECK(zero_arm.find("quot") == std::string::npos);
    CHECK(zero_arm.find("rem") == std::string::npos);
    CHECK(zero_arm.find("/") == std::string::npos);
    CHECK(zero_arm.find("-dividend") == std::string::npos);

    const std::string min_cond =
        std::string("else if (dividend == ") + std::string(min_name) + " && divisor == -1)";
    const auto min_if = body.find(min_cond);
    REQUIRE(min_if != std::string::npos);
    CHECK(zero_if < min_if);
    const auto min_arm = brace_block_after(body, min_if);
    REQUIRE(!min_arm.empty());
    const auto min_check = min_arm.find("__prism_check(0, \"FUNC-CONTRACT\", \"");
    const auto min_trap = min_arm.find("__builtin_trap()");
    REQUIRE(min_check != std::string::npos);
    REQUIRE(min_trap != std::string::npos);
    CHECK(min_check < min_trap);
    CHECK(min_arm.find(min_msg) != std::string::npos);
    CHECK(min_arm.find("return") == std::string::npos);
    CHECK(min_arm.find("quot") == std::string::npos);
    CHECK(min_arm.find("rem") == std::string::npos);
    CHECK(min_arm.find("/") == std::string::npos);
    CHECK(min_arm.find("-dividend") == std::string::npos);
    CHECK(min_arm.find(std::string("-") + std::string(min_name)) == std::string::npos);

    const auto else_at = body.find("} else {", min_if);
    REQUIRE(else_at != std::string::npos);
    const auto else_arm = brace_block_after(body, else_at);
    REQUIRE(!else_arm.empty());
    CHECK(else_arm.find(std::string(result_type) + " r;") != std::string::npos);
    CHECK(else_arm.find("r.quot = dividend / divisor;") != std::string::npos);
    CHECK(else_arm.find("r.rem = dividend - r.quot * divisor;") != std::string::npos);
    CHECK(else_arm.find("return r;") != std::string::npos);
    CHECK(else_arm.find(min_name) == std::string::npos);
    CHECK(else_arm.find("%") == std::string::npos);
    CHECK(else_arm.find("-dividend") == std::string::npos);
    CHECK(else_arm.find("__prism_check") == std::string::npos);
    CHECK(else_arm.find("__builtin_trap") == std::string::npos);

    CHECK(body.find("return r;") != std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(std::count(body.begin(), body.end(), '/') == 1);
}

}  // namespace

TEST_CASE("stdlib model: div traps on zero and the minimum and otherwise truncates toward zero") {
    const auto src = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" /
                           "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    CHECK(src.find("#include <limits.h>") != std::string::npos);

    const auto div_at = src.find("div_t div(int dividend, int divisor)");
    const auto ldiv_at = src.find("ldiv_t ldiv(long dividend, long divisor)");
    const auto lldiv_at = src.find("lldiv_t lldiv(long long dividend, long long divisor)");
    const auto llabs_at = src.find("long long llabs(long long x)");
    const auto atoi_at = src.find("int atoi(const char *s)");
    REQUIRE(div_at != std::string::npos);
    REQUIRE(ldiv_at != std::string::npos);
    REQUIRE(lldiv_at != std::string::npos);
    REQUIRE(llabs_at != std::string::npos);
    REQUIRE(atoi_at != std::string::npos);
    CHECK(llabs_at < div_at);
    CHECK(div_at < ldiv_at);
    CHECK(ldiv_at < lldiv_at);
    CHECK(lldiv_at < atoi_at);

    const auto note_at = src.rfind("/*", div_at);
    REQUIRE(note_at != std::string::npos);
    CHECK(src.rfind("/*", ldiv_at) == note_at);
    CHECK(src.rfind("/*", lldiv_at) == note_at);
    const auto note = src.substr(note_at, div_at - note_at);
    CHECK(note.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(note.find("__builtin_trap") != std::string::npos);
    CHECK(note.find("does not return a quotient or a remainder") != std::string::npos);
    CHECK(note.find("do not negate the minimum") != std::string::npos);
    CHECK(note.find("do not return a wrapped quotient") != std::string::npos);
    CHECK(note.find("toward-zero") != std::string::npos);
    CHECK(note.find("truncated quotient") != std::string::npos);
    CHECK(note.find("dividend - quot * divisor") != std::string::npos);
    CHECK(note.find("A remainder is not invented") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("typedef struct { int quot; int rem; } div_t;") != std::string::npos);
    CHECK(note.find("typedef struct { long quot; long rem; } ldiv_t;") != std::string::npos);
    CHECK(note.find("typedef struct { long long quot; long long rem; } lldiv_t;") != std::string::npos);

    check_div(src, "div_t div(int dividend, int divisor)", "INT_MIN", "div_t", "div by zero is undefined",
              "div(INT_MIN, -1) is undefined");
    check_div(src, "ldiv_t ldiv(long dividend, long divisor)", "LONG_MIN", "ldiv_t", "ldiv by zero is undefined",
              "ldiv(LONG_MIN, -1) is undefined");
    check_div(src, "lldiv_t lldiv(long long dividend, long long divisor)", "LLONG_MIN", "lldiv_t",
              "lldiv by zero is undefined", "lldiv(LLONG_MIN, -1) is undefined");

    CHECK(src.find("__prism_check(0, \"FUNC-CONTRACT\", \"abs(INT_MIN) is undefined\")") != std::string::npos);
    CHECK(src.find("__prism_check(0, \"FUNC-CONTRACT\", \"labs(LONG_MIN) is undefined\")") != std::string::npos);
    CHECK(src.find("__prism_check(0, \"FUNC-CONTRACT\", \"llabs(LLONG_MIN) is undefined\")") != std::string::npos);
    CHECK(src.find("void *malloc(size_t n)") != std::string::npos);
    CHECK(src.find("void *calloc(size_t n, size_t sz)") != std::string::npos);
    CHECK(src.find("void free(void *p)") != std::string::npos);
    CHECK(src.find("void *realloc(void *p, size_t n)") != std::string::npos);
    CHECK(src.find("int atoi(const char *s)") != std::string::npos);
    CHECK(src.find("long atol(const char *s)") != std::string::npos);
    CHECK(src.find("long strtol(const char *s, char **end, int base)") != std::string::npos);
    CHECK(src.find("int rand(void)") != std::string::npos);
    CHECK(src.find("void srand(unsigned s)") != std::string::npos);
    CHECK(src.find("char *getenv(const char *name)") != std::string::npos);
    CHECK(src.find("void qsort(") != std::string::npos);
    CHECK(src.find("void *bsearch(") != std::string::npos);
    CHECK(src.find("void *operator new(size_t n)") != std::string::npos);
    CHECK(src.find("void operator delete(void *p) noexcept") != std::string::npos);
    CHECK(src.find("the seed is not stored") != std::string::npos);
}

TEST_CASE("div contracts: harness includes stdlib.cpp and calls div, ldiv, and lldiv") {
    const auto cpp = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "libc-models" /
                           "stdlib_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/stdlib.cpp\"") != std::string::npos);
    CHECK(cpp.find("#include \"harness.h\"") != std::string::npos);

    const auto true_body = function_body(cpp, "int div_quot_true(void)");
    REQUIRE(!true_body.empty());
    CHECK(true_body.find("div(") != std::string::npos);
    CHECK(true_body.find("ldiv(") != std::string::npos);
    CHECK(true_body.find("lldiv(") != std::string::npos);
    CHECK(true_body.find("assert(d.quot == 2);") != std::string::npos);
    CHECK(true_body.find("assert(d.rem == 7 - d.quot * 3);") != std::string::npos);
    CHECK(true_body.find("assert(n.quot == -2);") != std::string::npos);
    CHECK(true_body.find("assert(n.rem == -7 - n.quot * 3);") != std::string::npos);
    CHECK(true_body.find("assert(p.quot == -2);") != std::string::npos);
    CHECK(true_body.find("assert(p.rem == 7 - p.quot * -3);") != std::string::npos);
    CHECK(true_body.find("assert(b.quot == 2);") != std::string::npos);
    CHECK(true_body.find("assert(b.rem == -7 - b.quot * -3);") != std::string::npos);
    CHECK(true_body.find("assert(z.quot == 0);") != std::string::npos);
    CHECK(true_body.find("assert(z.rem == 1);") != std::string::npos);
    CHECK(true_body.find("assert(ld.quot == -2L);") != std::string::npos);
    CHECK(true_body.find("assert(ld.rem == -7L - ld.quot * 3L);") != std::string::npos);
    CHECK(true_body.find("assert(lld.quot == -2LL);") != std::string::npos);
    CHECK(true_body.find("assert(lld.rem == -7LL - lld.quot * 3LL);") != std::string::npos);

    for (const char* sig : {"int div_rem_false(void)", "int ldiv_rem_false(void)", "int lldiv_rem_false(void)"}) {
        const auto body = function_body(cpp, sig);
        REQUIRE(!body.empty());
        CHECK(body.find("assert(") != std::string::npos);
    }
    CHECK(cpp.find("assert(d.rem == 2);") != std::string::npos);
    CHECK(cpp.find("assert(d.rem == 2L);") != std::string::npos);
    CHECK(cpp.find("assert(d.rem == 2LL);") != std::string::npos);
    CHECK(cpp.find("int abs_intmin_false(int x)") != std::string::npos);
}

TEST_CASE("div contracts: yml true task and false asserts expect FUNC-CONTRACT") {
    const auto yml = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "libc-models" /
                           "stdlib_contracts.yml");
    const auto cpp = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "conformance" / "libc-models" /
                           "stdlib_contracts.cpp");
    REQUIRE(!yml.empty());
    REQUIRE(!cpp.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: stdlib_contracts.cpp") != std::string::npos);
    CHECK(yml.find("language: C") != std::string::npos);
    CHECK(yml.find("property: libc-contract") != std::string::npos);

    enum class Sec { None, Expected, Class };
    Sec sec = Sec::None;
    std::map<std::string, bool> expected;
    std::map<std::string, std::string> classes;
    std::istringstream in(yml);
    std::string line;
    while (std::getline(in, line)) {
        line = strip_hash_comment(line);
        if (line.empty()) continue;
        if (line == "expected:") {
            sec = Sec::Expected;
            continue;
        }
        if (line.rfind("expect_class:", 0) == 0) {
            sec = Sec::Class;
            continue;
        }
        if (sec == Sec::None) continue;
        const auto colon = line.find(':');
        REQUIRE(colon != std::string::npos);
        const auto key = trim(line.substr(0, colon));
        auto val = trim(line.substr(colon + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (sec == Sec::Expected) {
            REQUIRE((val == "true" || val == "false"));
            expected[key] = val == "true";
        } else {
            classes[key] = val;
        }
    }

    const std::set<std::string> ours_false = {"div_rem_false", "ldiv_rem_false", "lldiv_rem_false"};
    REQUIRE(expected.count("div_quot_true"));
    CHECK(expected["div_quot_true"]);
    CHECK(cpp.find("int div_quot_true(") != std::string::npos);
    for (const auto& name : ours_false) {
        CAPTURE(name);
        REQUIRE(expected.count(name));
        CHECK_FALSE(expected[name]);
        CHECK(cpp.find("int " + name + "(") != std::string::npos);
        REQUIRE(classes.count(name));
        CHECK(classes[name] == "FUNC-CONTRACT");
        CHECK(classes[name] != "PROVED");
        CHECK(classes[name] != "CLEAN");
    }
    CHECK(expected.count("abs_intmin_false"));
    CHECK(classes.count("abs_intmin_false"));
    CHECK(classes["abs_intmin_false"] == "INT-SIGNED-OVF");
}
