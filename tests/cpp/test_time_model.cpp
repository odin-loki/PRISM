// time, mktime, difftime, clock, clock_gettime, gmtime, and localtime models. Reads time.cpp as text. Does not compile the model.
// docs/finish-notes/time.md
// docs/finish-notes/difftime.md
// docs/finish-notes/clock.md
// docs/finish-notes/clock-gettime.md
// docs/finish-notes/gmtime.md
// docs/finish-notes/localtime.md

#include <doctest/doctest.h>

#include "test_support.hpp"

#include "prose_text.hpp"

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
    CHECK(prism_test::first_after(body, "tp->tm_mon", nullg));
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
    const auto src = prism_test::unwrap_prose(slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp"));
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
    CHECK(prism_test::first_before(body, "__builtin_trap()", ret));

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto nullg = mktime_body.find("if (tp == 0)");
    REQUIRE(nullg != std::string::npos);
    CHECK(prism_test::first_after(mktime_body, "tp->tm_mon", nullg));
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

TEST_CASE("clock model: nondet clock_t does not mention tm_mon") {
    const auto src = prism_test::unwrap_prose(slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp"));
    REQUIRE(!src.empty());
    CHECK(src.find("typedef long clock_t;") != std::string::npos);
    const auto at = src.find("clock_t clock(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("nondet clock_t") != std::string::npos);
    CHECK(note.find("does not return a constant") != std::string::npos);
    CHECK(note.find("does not read a struct tm") != std::string::npos);
    CHECK(note.find("not a proof of elapsed processor time") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("tm_mon") == std::string::npos);

    const auto body = function_body(src, "clock_t clock(");
    REQUIRE(!body.empty());
    CHECK(body.find("__VERIFIER_nondet_long") != std::string::npos);
    CHECK(body.find("return v") != std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("tm_mon") == std::string::npos);
    CHECK(body.find("tm_") == std::string::npos);

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto nullg = mktime_body.find("if (tp == 0)");
    REQUIRE(nullg != std::string::npos);
    CHECK(prism_test::first_after(mktime_body, "tp->tm_mon", nullg));

    const auto difftime_body = function_body(src, "double difftime(");
    REQUIRE(!difftime_body.empty());
    CHECK(difftime_body.find("__builtin_sub_overflow(time1, time0, &diff)") != std::string::npos);
    CHECK(difftime_body.find("__builtin_trap()") != std::string::npos);
    CHECK(difftime_body.find("return 0") == std::string::npos);

    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.yml");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    CHECK(cpp.find("clock(") != std::string::npos);
    CHECK(yml.find("clock") == std::string::npos);
}

TEST_CASE("clock_gettime model: null timespec returns -1 before any store") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("int clock_gettime(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not a proof of the current time") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "int clock_gettime(");
    REQUIRE(!body.empty());
    const auto nullg = body.find("if (tp == 0)");
    REQUIRE(nullg != std::string::npos);
    const auto arm = brace_block_after(body, nullg);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(0, \"PTR-NULL-DEREF\", \"clock_gettime null timespec\")");
    const auto ret = arm.find("return -1");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(arm.find("tv_sec") == std::string::npos);
    CHECK(arm.find("tv_nsec") == std::string::npos);
    CHECK(arm.find("tp->") == std::string::npos);

    const auto ret_at = body.find("return -1");
    const auto sec = body.find("tp->tv_sec = __VERIFIER_nondet_long()");
    const auto nsec = body.find("long nsec = __VERIFIER_nondet_long()");
    const auto assume = body.find("__prism_assume(nsec >= 0 && nsec <= 999999999)");
    const auto nstore = body.find("tp->tv_nsec = nsec");
    REQUIRE(ret_at != std::string::npos);
    REQUIRE(sec != std::string::npos);
    REQUIRE(nsec != std::string::npos);
    REQUIRE(assume != std::string::npos);
    REQUIRE(nstore != std::string::npos);
    CHECK(nullg < ret_at);
    CHECK(ret_at < sec);
    CHECK(sec < nsec);
    CHECK(nsec < assume);
    CHECK(assume < nstore);
    const auto ok = body.find("return 0");
    REQUIRE(ok != std::string::npos);
    CHECK(nstore < ok);

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto mnull = mktime_body.find("if (tp == 0)");
    REQUIRE(mnull != std::string::npos);
    CHECK(prism_test::first_after(mktime_body, "tp->tm_mon", mnull));

    const auto time_body = function_body(src, "time_t time(");
    const auto difftime_body = function_body(src, "double difftime(");
    const auto clock_body = function_body(src, "clock_t clock(");
    REQUIRE(!time_body.empty());
    REQUIRE(!difftime_body.empty());
    REQUIRE(!clock_body.empty());
    CHECK(time_body.find("if (t != 0)") != std::string::npos);
    CHECK(difftime_body.find("__builtin_sub_overflow(time1, time0, &diff)") != std::string::npos);
    CHECK(clock_body.find("return v") != std::string::npos);
    CHECK(clock_body.find("tv_nsec") == std::string::npos);

    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.yml");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    CHECK(cpp.find("clock_gettime(") != std::string::npos);
    CHECK(cpp.find("clock(") != std::string::npos);
    CHECK(yml.find("clock_gettime") == std::string::npos);
    CHECK(yml.find("clock") == std::string::npos);
}

TEST_CASE("gmtime model: null time_t returns 0 before any tm field is written") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("struct tm *gmtime(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not a proof of the calendar time") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "struct tm *gmtime(");
    REQUIRE(!body.empty());
    const auto nullg = body.find("if (timer == 0)");
    REQUIRE(nullg != std::string::npos);
    const auto arm = brace_block_after(body, nullg);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(0, \"PTR-NULL-DEREF\", \"gmtime null time_t\")");
    const auto ret = arm.find("return 0");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(arm.find("tm_") == std::string::npos);
    CHECK(arm.find("storage") == std::string::npos);

    const auto ret_at = body.find("return 0");
    const auto sec = body.find("storage.tm_sec = sec");
    const auto minute = body.find("storage.tm_min = min");
    const auto hour = body.find("storage.tm_hour = hour");
    const auto mday_store = body.find("storage.tm_mday = mday");
    const auto mon_store = body.find("storage.tm_mon = mon");
    const auto year = body.find("storage.tm_year = year");
    const auto wday = body.find("storage.tm_wday = wday");
    const auto yday = body.find("storage.tm_yday = yday");
    const auto isdst = body.find("storage.tm_isdst = isdst");
    const auto mon_assume = body.find("__prism_assume(mon >= 0 && mon <= 11)");
    const auto mday_assume = body.find("__prism_assume(mday >= 1 && mday <= 31)");
    REQUIRE(ret_at != std::string::npos);
    REQUIRE(sec != std::string::npos);
    REQUIRE(minute != std::string::npos);
    REQUIRE(hour != std::string::npos);
    REQUIRE(mday_store != std::string::npos);
    REQUIRE(mon_store != std::string::npos);
    REQUIRE(year != std::string::npos);
    REQUIRE(wday != std::string::npos);
    REQUIRE(yday != std::string::npos);
    REQUIRE(isdst != std::string::npos);
    REQUIRE(mon_assume != std::string::npos);
    REQUIRE(mday_assume != std::string::npos);
    CHECK(nullg < ret_at);
    CHECK(ret_at < mon_assume);
    CHECK(mon_assume < mday_assume);
    CHECK(mday_assume < sec);
    CHECK(sec < minute);
    CHECK(minute < hour);
    CHECK(hour < mday_store);
    CHECK(mday_store < mon_store);
    CHECK(mon_store < year);
    CHECK(year < wday);
    CHECK(wday < yday);
    CHECK(yday < isdst);
    CHECK(body.find("__VERIFIER_nondet_int") != std::string::npos);
    CHECK(body.find("1970") == std::string::npos);
    CHECK(body.find("86400") == std::string::npos);
    CHECK(body.find("*timer") == std::string::npos);
    const auto ok = body.find("return &storage");
    REQUIRE(ok != std::string::npos);
    CHECK(isdst < ok);

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto mnull = mktime_body.find("if (tp == 0)");
    REQUIRE(mnull != std::string::npos);
    CHECK(prism_test::first_after(mktime_body, "tp->tm_mon", mnull));

    const auto time_body = function_body(src, "time_t time(");
    const auto difftime_body = function_body(src, "double difftime(");
    const auto clock_body = function_body(src, "clock_t clock(");
    const auto clock_gettime_body = function_body(src, "int clock_gettime(");
    REQUIRE(!time_body.empty());
    REQUIRE(!difftime_body.empty());
    REQUIRE(!clock_body.empty());
    REQUIRE(!clock_gettime_body.empty());
    CHECK(time_body.find("if (t != 0)") != std::string::npos);
    CHECK(time_body.find("return v") != std::string::npos);
    CHECK(difftime_body.find("__builtin_sub_overflow(time1, time0, &diff)") != std::string::npos);
    CHECK(difftime_body.find("__builtin_trap()") != std::string::npos);
    CHECK(clock_body.find("return v") != std::string::npos);
    CHECK(clock_body.find("tm_mon") == std::string::npos);
    CHECK(clock_gettime_body.find("__prism_check(0, \"PTR-NULL-DEREF\", \"clock_gettime null timespec\")") !=
          std::string::npos);
    CHECK(clock_gettime_body.find("__prism_assume(nsec >= 0 && nsec <= 999999999)") != std::string::npos);
    CHECK(clock_gettime_body.find("tm_mon") == std::string::npos);

    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.yml");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    CHECK(cpp.find("gmtime(") != std::string::npos);
    CHECK(cpp.find("(void)gmtime(&when)") != std::string::npos);
    CHECK(yml.find("gmtime") == std::string::npos);
    CHECK(yml.find("1970") == std::string::npos);
}

TEST_CASE("localtime model: null time_t returns 0 before any tm field is written") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "time.cpp");
    REQUIRE(!src.empty());
    const auto at = src.find("struct tm *localtime(");
    REQUIRE(at != std::string::npos);
    const auto note_at = src.rfind("/*", at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, at - note_at);
    CHECK(note.find("not a proof of the local calendar and not a proof of a timezone") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "struct tm *localtime(");
    REQUIRE(!body.empty());
    const auto nullg = body.find("if (timer == 0)");
    REQUIRE(nullg != std::string::npos);
    const auto arm = brace_block_after(body, nullg);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(0, \"PTR-NULL-DEREF\", \"localtime null time_t\")");
    const auto ret = arm.find("return 0");
    REQUIRE(check != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < ret);
    CHECK(arm.find("tm_") == std::string::npos);
    CHECK(arm.find("storage") == std::string::npos);

    const auto ret_at = body.find("return 0");
    const auto nondet = body.find("__VERIFIER_nondet_int");
    const auto sec = body.find("storage.tm_sec = sec");
    const auto minute = body.find("storage.tm_min = min");
    const auto hour = body.find("storage.tm_hour = hour");
    const auto mday_store = body.find("storage.tm_mday = mday");
    const auto mon_store = body.find("storage.tm_mon = mon");
    const auto year = body.find("storage.tm_year = year");
    const auto wday = body.find("storage.tm_wday = wday");
    const auto yday = body.find("storage.tm_yday = yday");
    const auto isdst = body.find("storage.tm_isdst = isdst");
    REQUIRE(ret_at != std::string::npos);
    REQUIRE(nondet != std::string::npos);
    REQUIRE(sec != std::string::npos);
    REQUIRE(minute != std::string::npos);
    REQUIRE(hour != std::string::npos);
    REQUIRE(mday_store != std::string::npos);
    REQUIRE(mon_store != std::string::npos);
    REQUIRE(year != std::string::npos);
    REQUIRE(wday != std::string::npos);
    REQUIRE(yday != std::string::npos);
    REQUIRE(isdst != std::string::npos);
    CHECK(nullg < ret_at);
    CHECK(ret_at < nondet);
    CHECK(nondet < sec);
    CHECK(sec < minute);
    CHECK(minute < hour);
    CHECK(hour < mday_store);
    CHECK(mday_store < mon_store);
    CHECK(mon_store < year);
    CHECK(year < wday);
    CHECK(wday < yday);
    CHECK(yday < isdst);
    CHECK(body.find("__prism_assume") == std::string::npos);
    CHECK(body.find("1970") == std::string::npos);
    CHECK(body.find("86400") == std::string::npos);
    CHECK(body.find("*timer") == std::string::npos);
    CHECK(body.find("timezone") == std::string::npos);
    CHECK(body.find("tzset") == std::string::npos);
    CHECK(body.find("tm_gmtoff") == std::string::npos);
    CHECK(body.find("tm_zone") == std::string::npos);
    const auto ok = body.find("return &storage");
    REQUIRE(ok != std::string::npos);
    CHECK(isdst < ok);

    const auto mktime_body = function_body(src, "time_t mktime(");
    REQUIRE(!mktime_body.empty());
    const auto mnull = mktime_body.find("if (tp == 0)");
    REQUIRE(mnull != std::string::npos);
    CHECK(prism_test::first_after(mktime_body, "tp->tm_mon", mnull));

    const auto time_body = function_body(src, "time_t time(");
    const auto difftime_body = function_body(src, "double difftime(");
    const auto clock_body = function_body(src, "clock_t clock(");
    const auto clock_gettime_body = function_body(src, "int clock_gettime(");
    const auto gmtime_body = function_body(src, "struct tm *gmtime(");
    REQUIRE(!time_body.empty());
    REQUIRE(!difftime_body.empty());
    REQUIRE(!clock_body.empty());
    REQUIRE(!clock_gettime_body.empty());
    REQUIRE(!gmtime_body.empty());
    CHECK(time_body.find("if (t != 0)") != std::string::npos);
    CHECK(time_body.find("return v") != std::string::npos);
    CHECK(difftime_body.find("__builtin_sub_overflow(time1, time0, &diff)") != std::string::npos);
    CHECK(difftime_body.find("__builtin_trap()") != std::string::npos);
    CHECK(clock_body.find("return v") != std::string::npos);
    CHECK(clock_body.find("tm_mon") == std::string::npos);
    CHECK(clock_gettime_body.find("__prism_check(0, \"PTR-NULL-DEREF\", \"clock_gettime null timespec\")") !=
          std::string::npos);
    CHECK(clock_gettime_body.find("__prism_assume(nsec >= 0 && nsec <= 999999999)") != std::string::npos);
    CHECK(gmtime_body.find("__prism_check(0, \"PTR-NULL-DEREF\", \"gmtime null time_t\")") != std::string::npos);
    CHECK(gmtime_body.find("__prism_assume(mon >= 0 && mon <= 11)") != std::string::npos);
    CHECK(gmtime_body.find("__prism_assume(mday >= 1 && mday <= 31)") != std::string::npos);
    CHECK(gmtime_body.find("localtime") == std::string::npos);

    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.cpp");
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "time_contracts.yml");
    REQUIRE(!cpp.empty());
    REQUIRE(!yml.empty());
    CHECK(cpp.find("localtime(") != std::string::npos);
    CHECK(cpp.find("(void)localtime(&when)") != std::string::npos);
    CHECK(cpp.find("1970") == std::string::npos);
    CHECK(yml.find("localtime") == std::string::npos);
    CHECK(yml.find("1970") == std::string::npos);
}
