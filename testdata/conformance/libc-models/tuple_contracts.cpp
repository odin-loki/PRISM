// Names <tuple> so the C++ model-header conformance case sees this include.
// The scored read is testdata/conformance/prism/cxx/cxx_tuple_true.cpp.
// There is no false twin: an index that is not a compile-time element is
// ill-formed, so it is not a defect finding.
#include <tuple>

int tuple_get_stored(int c) {
    std::tuple<int> t(c);
    return std::get<0>(t);
}
