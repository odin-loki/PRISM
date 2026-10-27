// PRISM conformance task cxx/cxx_any_true.cpp: expected true (noassert)
// any_cast<int> of an any that holds an int
#include <any>
int cxx_any_true(int c) {
    std::any a(c);
    return std::any_cast<int>(a);
}
