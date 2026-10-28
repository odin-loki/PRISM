// time, mktime, and difftime models. Reads time.cpp as text. Does not compile the model.
// docs/finish-notes/time.md
// docs/finish-notes/difftime.md

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

}  // namespace

TEST_CASE("time model: nondet value, null stores nothing") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("time_t time(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("time is not PROVED and not CLEAN") != std::string::npos);

    const auto body = function_body(src, "time_t time(");
    REQUIRE(!body.empty());
    CHECK(body.find("__VERIFIER_nondet_long") != std::string::npos);
    const auto guard = body.find("if (t != 0)");
    REQUIRE(guard != std::string::npos);
    CHECK(body.substr(0, guard).find("*t") == std::string::npos);
    const auto arm = brace_block_after(body, guard);
    REQUIRE(!arm.empty());
    CHECK(arm.find("*t = v") != std::string::npos);
    CHECK(body.find("return v") != std::string::npos);
    CHECK(src.find("strftime") == std::string::npos);
    CHECK(src.find("clock") == std::string::npos);
}

TEST_CASE("mktime model: month 99 is not a successful calendar time") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("time_t mktime(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not normalized into a later year") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "time_t mktime(");
    REQUIRE(!body.empty());
    const auto nullg = body.find("if (tp == 0)");
    REQUIRE(nullg != std::string::npos);
    CHECK(nullg < body.find("tp->tm_mon"));
    const auto cond = body.find("if (mon < 0 || mon > 11 || mday < 1 || mday > 31)");
    REQUIRE(cond != std::string::npos);
    CHECK(body.find("tp->tm_mon") != std::string::npos);
    CHECK(body.find("tp->tm_mday") != std::string::npos);
    CHECK(body.find("tm_sec") == std::string::npos);
    CHECK(body.find("tm_min") == std::string::npos);
    CHECK(body.find("tm_hour") == std::string::npos);
    CHECK(body.find("tm_year") == std::string::npos);
    CHECK(body.find("tm_wday") == std::string::npos);
    CHECK(body.find("tm_yday") == std::string::npos);
    CHECK(body.find("tm_isdst") == std::string::npos);
    CHECK(body.find("1970") == std::string::npos);
    CHECK(body.find("86400") == std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("/ 12") == std::string::npos);

    const auto arm = brace_block_after(body, cond);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(");
    const auto ret = arm.find("return (time_t)-1");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(arm.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
    CHECK(arm.find("tm_") == std::string::npos);

    const auto arm_at = body.find('{', cond);
    REQUIRE(arm_at != std::string::npos);
    const auto after = body.substr(arm_at + arm.size());
    CHECK(after.find("__prism_assume(0)") != std::string::npos);
    CHECK(after.find("return 0") == std::string::npos);
    CHECK(after.find("return (time_t)-1") != std::string::npos);
}

TEST_CASE("difftime model: a difference that fits is a double, and one that does not traps") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("double difftime(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not a calendar proof") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "double difftime(");
    REQUIRE(!body.empty());
    CHECK(body.find("__builtin_sub_overflow(time1, time0, &diff)") != std::string::npos);
    CHECK(body.find("time1 - time0") == std::string::npos);
    CHECK(body.find("tm_") == std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("return 0.0") == std::string::npos);

    const auto cond = body.find("if (__builtin_sub_overflow(time1, time0, &diff))");
    REQUIRE(cond != std::string::npos);
    const auto arm = brace_block_after(body, cond);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(0, \"FUNC-CONTRACT\", \"difftime() difference does not fit in time_t\")");
    const auto trap = arm.find("__builtin_trap()");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(check < trap);
    CHECK(arm.find("return") == std::string::npos);

    const auto ret = body.find("return (double)diff");
    REQUIRE(ret != std::string::npos);
    CHECK(body.find("__builtin_trap()") < ret);

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto nullg = mktime_body.find("if (tp == 0)");
    REQUIRE(nullg != std::string::npos);
    CHECK(nullg < mktime_body.find("tp->tm_mon"));
}

TEST_CASE("difftime harness: fitting difference is true and a zero stand-in is FUNC-CONTRACT") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.yml");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/time.cpp\"") != std::string::npos);
    CHECK(cpp.find("int difftime_fit_true(") != std::string::npos);
    CHECK(cpp.find("assert(difftime((time_t)5, (time_t)3) == 2.0)") != std::string::npos);
    CHECK(cpp.find("assert(difftime((time_t)3, (time_t)5) == -2.0)") != std::string::npos);
    CHECK(cpp.find("assert(difftime((time_t)4, (time_t)4) == 0.0)") != std::string::npos);
    CHECK(cpp.find("int difftime_nofit_false(") != std::string::npos);
    CHECK(cpp.find("assert(difftime((time_t)LONG_MAX, (time_t)LONG_MIN) == 0.0)") != std::string::npos);
    CHECK(yml.find("difftime_fit_true: true") != std::string::npos);
    CHECK(yml.find("difftime_nofit_false: false") != std::string::npos);
    CHECK(yml.find("difftime_nofit_false: \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(yml.find("mktime_month99_false: \"FUNC-CONTRACT\"") != std::string::npos);
}
