// PRISM conformance task cxx/cxx_regex_locale_true.cpp: expected true (noassert)
// imbue a named locale: \w uses its ctype facet, getloc returns it
#include <cassert>
#include <locale>
#include <regex>
int cxx_regex_locale_true(int c) {
    std::regex r;
    r.imbue(std::locale::classic());
    r.assign("\\w+");
    assert(r.getloc() == std::locale::classic());
    assert(std::regex_match((c & 1) ? "ab_1" : "Z9", r));
    return 0;
}
