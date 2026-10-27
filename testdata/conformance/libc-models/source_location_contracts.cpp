// Names <source_location> so the C++ model-header conformance case sees
// this include. The scored task is
// testdata/conformance/prism/cxx/cxx_source_location_true.cpp.
// There is no false twin and no yml: this header does not plant a defect.
#include <source_location>

int source_location_line_true(int n) {
    const std::source_location loc = std::source_location::current();
    if (loc.line() == 0) return n + 1;
    return n;
}
