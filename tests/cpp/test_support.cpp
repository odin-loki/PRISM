// NOTRUN registry and listener, and the ordering helpers (test_support.hpp).
#include "test_support.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <ostream>
#include <set>
#include <string>

namespace prism_test {

// Set by the listener below; not in the header (only the listener calls it).
void current_case_name(const char* name);

namespace {

std::vector<NotrunRecord>& records() {
    static std::vector<NotrunRecord> r;
    return r;
}

// The name of the running test case, set by the listener below.
std::string& current_case() {
    static std::string name;
    return name;
}

}  // namespace

void current_case_name(const char* name) { current_case() = name ? name : ""; }

bool require_tools_value(const char* value) {
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

bool require_tools() { return require_tools_value(std::getenv("PRISM_TESTS_REQUIRE_TOOLS")); }

void notrun(std::string_view reason) {
    // Engine reasons (frontend_usable, ...) already start with "NOTRUN: ".
    if (reason.starts_with("NOTRUN: ")) reason.remove_prefix(8);
    const std::string why(reason);
    records().push_back({current_case(), why});
    if (require_tools()) {
        FAIL_CHECK("NOTRUN (PRISM_TESTS_REQUIRE_TOOLS=1 makes this a failure): " << why);
    } else {
        MESSAGE("NOTRUN: " << why);
    }
}

const std::vector<NotrunRecord>& notrun_records() { return records(); }

bool appears_before(std::string_view text, std::string_view a, std::string_view b) {
    const auto pa = text.find(a);
    const auto pb = text.find(b);
    return pa != std::string_view::npos && pb != std::string_view::npos && pa < pb;
}

bool first_after(std::string_view text, std::string_view needle, std::size_t pos) {
    const auto p = text.find(needle);
    return p != std::string_view::npos && pos != std::string_view::npos && p > pos;
}

bool first_before(std::string_view text, std::string_view needle, std::size_t pos) {
    const auto p = text.find(needle);
    return p != std::string_view::npos && pos != std::string_view::npos && p < pos;
}

}  // namespace prism_test

namespace {

// Names each test case for notrun(), and at the end of the run prints how many
// cases were NOTRUN and why, so a NOTRUN never hides inside the passed count.
struct NotrunListener : doctest::IReporter {
    std::ostream& out;
    explicit NotrunListener(const doctest::ContextOptions& o) : out(*o.cout) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {
        const auto& recs = prism_test::notrun_records();
        std::set<std::string> cases;
        std::set<std::string> reasons;
        for (const auto& r : recs) {
            cases.insert(r.test_case);
            reasons.insert(r.reason);
        }
        out << "NOTRUN: " << cases.size() << " test case" << (cases.size() == 1 ? "" : "s");
        if (prism_test::require_tools() && !cases.empty()) out << " (PRISM_TESTS_REQUIRE_TOOLS=1: each one failed)";
        out << (reasons.empty() ? "\n" : ", reasons:\n");
        for (const auto& why : reasons) {
            std::size_t n = 0;
            for (const auto& r : recs)
                if (r.reason == why) ++n;
            out << "  NOTRUN x" << n << ": " << why << "\n";
        }
        out.flush();
    }
    void test_case_start(const doctest::TestCaseData& tc) override { prism_test::current_case_name(tc.m_name); }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace

DOCTEST_REGISTER_LISTENER("prism_notrun", 2, NotrunListener);

TEST_CASE("test support: appears_before needs both strings, in order") {
    CHECK(prism_test::appears_before("alpha beta", "alpha", "beta"));
    CHECK_FALSE(prism_test::appears_before("alpha beta", "beta", "alpha"));
    // The vacuous form `find(a) < find(b)` holds here; this does not.
    CHECK_FALSE(prism_test::appears_before("alpha", "alpha", "beta"));
    CHECK_FALSE(prism_test::appears_before("beta", "alpha", "beta"));
    CHECK_FALSE(prism_test::appears_before("", "alpha", "beta"));
}

TEST_CASE("test support: first_after needs the string, after the position") {
    CHECK(prism_test::first_after("ab cd", "cd", 0));
    CHECK_FALSE(prism_test::first_after("ab cd", "ab", 0));
    // The vacuous form `find(x) > pos` holds here; this does not.
    CHECK_FALSE(prism_test::first_after("ab cd", "zz", 0));
    CHECK_FALSE(prism_test::first_after("ab cd", "cd", std::string::npos));
}

TEST_CASE("test support: first_before needs the string and a real bound") {
    CHECK(prism_test::first_before("ab cd", "ab", 3));
    CHECK_FALSE(prism_test::first_before("ab cd", "cd", 3));
    // The vacuous form `find(x) < pos` holds when pos is npos; this does not.
    CHECK_FALSE(prism_test::first_before("ab cd", "ab", std::string::npos));
    CHECK_FALSE(prism_test::first_before("ab cd", "zz", 3));
}

TEST_CASE("test support: PRISM_TESTS_REQUIRE_TOOLS values") {
    CHECK_FALSE(prism_test::require_tools_value(nullptr));
    CHECK_FALSE(prism_test::require_tools_value(""));
    CHECK_FALSE(prism_test::require_tools_value("0"));
    CHECK(prism_test::require_tools_value("1"));
    CHECK(prism_test::require_tools_value("yes"));
}
