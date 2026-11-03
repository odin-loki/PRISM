// PRISM conformance task cxx/cxx_regex_locale_false.cpp: expected false (noassert)
// imbue a named locale: \w uses its ctype facet; "Z-9" has a non-word '-'
#include <cassert>
#include <locale>
#include <regex>
int cxx_regex_locale_false(int c) {
    std::regex r;
    r.imbue(std::locale::classic());
    r.assign("\\w+");
    assert(r.getloc() == std::locale::classic());
    assert(std::regex_match((c & 1) ? "ab_1" : "Z-9", r));
    return 0;
}
