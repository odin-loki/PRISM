// Names <system_error> so the C++ model-header conformance case sees
// this include. The scored check is
// testdata/conformance/prism/cxx/cxx_system_error_true.cpp.
// There is no false twin: message() is omitted rather than stubbed, and
// this header does not plant a defect class of its own.
#include <system_error>

int system_error_value_true(int) {
    std::error_code ec(5, std::generic_category());
    if (ec.value() != 5) return 1;
    std::error_code def;
    if (def) return 1;
    return 0;
}
