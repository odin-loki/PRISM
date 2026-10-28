// PRISM conformance task cxx/cxx_regex_true.cpp: expected true (memsafety)
// std::regex owns a copy of the pattern characters. An empty pattern stays
// empty (no allocation, not a match-all pattern). A move leaves the source
// empty, and each destructor frees its own copy once. This is not a match.
#include <regex>
int cxx_regex_true(int) {
    std::regex pat("ab");
    if (pat.size() != 2 || pat.data() == nullptr) return 1;
    if (pat.data()[0] != 'a' || pat.data()[1] != 'b') return 1;
    std::regex empty("");
    if (!empty.empty() || empty.size() != 0 || empty.data() != nullptr) return 1;
    std::regex copy(pat);
    if (copy.size() != 2 || copy.data() == nullptr || copy.data() == pat.data()) return 1;
    if (copy.data()[0] != 'a' || copy.data()[1] != 'b') return 1;
    std::regex moved(std::move(pat));
    if (!pat.empty() || pat.size() != 0 || pat.data() != nullptr) return 1;
    if (moved.size() != 2 || moved.data()[0] != 'a' || moved.data()[1] != 'b') return 1;
    if (copy.size() != 2 || copy.data()[0] != 'a' || copy.data()[1] != 'b') return 1;
    return 0;
}
