// PRISM conformance task cxx/cxx_stacktrace_true.cpp: expected true (no-oob)
// current() has size 0 or 1. [0] is used only when that size is 1.
#include <stacktrace>
int cxx_stacktrace_true(int n) {
    std::stacktrace st = std::stacktrace::current();
    if (st.size() != 1) return n;
    if (st[0].native_handle() == nullptr) return 1;
    return n;
}
