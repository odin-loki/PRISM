// PRISM conformance task cxx/cxx_functional_false.cpp: expected false (memsafety)
// operator() on an empty std::function
#include <functional>
int cxx_functional_false(int c) {
    std::function<int(int)> f;
    if (c) return c;
    return f(c);
}
