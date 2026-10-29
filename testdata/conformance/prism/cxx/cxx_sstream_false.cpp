// PRISM conformance task cxx/cxx_sstream_false.cpp: expected false (no-uncaught)
// with badbit in exceptions(), setstate(badbit) throws std::ios_base::failure
// and nothing catches it
#include <sstream>

int cxx_sstream_false(int x) {
    std::ostringstream os;
    os << x;
    os.exceptions(std::ios_base::badbit);
    if (x == 6) os.setstate(std::ios_base::badbit);
    return os.str().empty() ? 0 : 1;
}
