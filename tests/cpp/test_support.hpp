// Helpers that keep a doctest case from passing when it did not check anything.
//
// notrun(reason): the case could not run its check here (a tool, a compiler
// runtime or a platform feature is missing). It prints "NOTRUN: reason",
// records the case in a registry, and a listener (test_support.cpp) prints
// "NOTRUN: N test cases" with the reasons at the end of the run. With
// PRISM_TESTS_REQUIRE_TOOLS=1 a NOTRUN is a failure instead (Law 1: a missing
// tool is never a clean result). Call it as the last statement of the case:
//     if (!c.ok) return prism_test::notrun("clang++ with ASan/UBSan not usable");
//
// appears_before / first_after / first_before: ordering checks that fail when
// a string is missing. `a.find(x) < a.find(y)` passes when y is missing (npos
// is the largest size_t) and `a.find(x) > pos` passes when x is missing.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace prism_test {

// One recorded NOTRUN: the test case it was in and why.
struct NotrunRecord {
    std::string test_case;
    std::string reason;
};

// True when PRISM_TESTS_REQUIRE_TOOLS is set to a value other than "" or "0".
bool require_tools();
// The rule require_tools() applies to the variable's value (nullptr = unset).
bool require_tools_value(const char* value);

// Mark the current test case NOTRUN (see the file comment).
void notrun(std::string_view reason);

// Every NOTRUN recorded so far in this run, in order.
const std::vector<NotrunRecord>& notrun_records();

// Both a and b occur in text, and the first a starts before the first b.
bool appears_before(std::string_view text, std::string_view a, std::string_view b);

// needle occurs in text, and its first occurrence starts after pos.
// (pos == npos never holds: there is nothing after the end.)
bool first_after(std::string_view text, std::string_view needle, std::size_t pos);

// needle occurs in text, and its first occurrence starts before pos.
// (pos == npos never holds: a missing bound is not "after everything".)
bool first_before(std::string_view text, std::string_view needle, std::size_t pos);

}  // namespace prism_test
