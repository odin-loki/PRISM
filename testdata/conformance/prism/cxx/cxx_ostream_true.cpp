// PRISM conformance task cxx/cxx_ostream_true.cpp: expected true (no-oob)
// Recorded character inside prism::cxx_model::ostream_buf. The header is
// included by path; <iostream> is not swapped.
#include "../../../../src/prism/pir/models/cxx/iostream/prism_ostream.hpp"

int cxx_ostream_true(int i) {
    prism::cxx_model::ostream_buf os;
    os << 42;
    if (os.failed() || os.size() != 2) return 0;
    if (i < 0 || i > 1) return 0;
    return static_cast<unsigned char>(os.data()[static_cast<std::size_t>(i)]);
}
