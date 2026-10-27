// PRISM conformance task cxx/cxx_stacktrace_false.cpp: expected false (no-oob)
// operator[] on an empty stacktrace (x == 0 leaves it empty)
#include <stacktrace>
int cxx_stacktrace_false(int x) {
    std::stacktrace st;
    if (x) return x;
    return st[0].native_handle() != nullptr ? 1 : 0;
}
