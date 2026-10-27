// PRISM conformance task cxx/cxx_ostream_false.cpp: expected false (no-oob)
// One past the heap block of prism::cxx_model::ostream_buf. The header is
// included by path; <iostream> is not swapped.
#include "../../../../src/prism/pir/models/cxx/iostream/prism_ostream.hpp"

int cxx_ostream_false(int i) {
    prism::cxx_model::ostream_buf os;
    os.write("0123456789abcdef", 16);  // past the 15-character local buffer
    if (os.failed()) return 0;
    if (i != 0) return 0;
    // The heap block is capacity() + 1 bytes. This index is one past it.
    return static_cast<unsigned char>(os.data()[os.capacity() + 1]);
}
