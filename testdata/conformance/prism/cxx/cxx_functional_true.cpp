// PRISM conformance task cxx/cxx_functional_true.cpp: expected true (memsafety)
// call the function pointer that was stored in std::function
#include <functional>
static int stored(int x) { return x; }
int cxx_functional_true(int c) {
    std::function<int(int)> f = stored;
    return f(c);
}
