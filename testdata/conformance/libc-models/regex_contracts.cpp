// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <regex> so the C++ model-header conformance case sees this include.
// The scored task is testdata/conformance/prism/cxx/cxx_regex_true.cpp.
// regex_match, regex_search, and regex_replace are not called. A true
// return from those would be a fake match. This header does not declare them.
#include <regex>

int regex_owns_true(int) {
    std::regex pat("ab");
    if (pat.size() != 2 || pat.data() == nullptr) return 1;
    if (pat.data()[0] != 'a' || pat.data()[1] != 'b') return 1;
    std::regex empty("");
    if (!empty.empty() || empty.size() != 0 || empty.data() != nullptr) return 1;
    std::regex copy(pat);
    if (copy.data() == pat.data()) return 1;
    std::regex moved(std::move(pat));
    if (!pat.empty() || moved.size() != 2 || moved.data()[1] != 'b') return 1;
    if (copy.data()[0] != 'a') return 1;
    return 0;
}
