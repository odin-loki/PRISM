// Names <typeindex> so the C++ model-header conformance case sees
// this include. The scored task is
// testdata/conformance/prism/cxx/cxx_typeindex_true.cpp.
// Comparing type_index is not a proof of the program. There is no
// false twin: this header does not plant a defect.
#include <typeindex>

int typeindex_same_type_true(int n) {
    const std::type_index a(typeid(int));
    const std::type_index b(typeid(int));
    const std::type_index c(typeid(double));
    int buf[1] = {n};
    if (a.name() == nullptr) return buf[0];
    if ((a == b) && !(a == c) && a.hash_code() == typeid(int).hash_code() && a.name() == typeid(int).name()) return buf[0];
    return buf[0];
}
