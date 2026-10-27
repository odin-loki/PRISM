// PRISM conformance task cxx/cxx_any_false.cpp: expected false (noassert)
// any_cast<int> of an empty any (c == 0), or of an any holding a double
#include <any>
int cxx_any_false(int c) {
    std::any a;
    if (c) a = 1.5;
    return std::any_cast<int>(a);
}
