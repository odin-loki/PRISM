// PRISM conformance task cxx/cxx_stacktrace_true.cpp: expected true (no-oob)
// current() has size 0 or 1. empty() is that size == 0. Two empty traces
// compare equal. A size of 1 is not equal to an empty trace. [0] is used
// only when size() is 1. This is not PROVED and not CLEAN.
#include <stacktrace>
int cxx_stacktrace_true(int n) {
    std::stacktrace st = std::stacktrace::current();
    std::stacktrace empty;
    if (st.size() == 0) {
        if (!st.empty()) return 1;
        if (!(st == empty)) return 1;
        if (st != empty) return 1;
        return n;
    }
    if (st.empty()) return 1;
    if (st.size() != 1) return n;
    if (st == empty) return 1;
    if (!(st != empty)) return 1;
    if (!(st == st)) return 1;
    if (st != st) return 1;
    if (st[0].native_handle() == nullptr) return 1;
    return n;
}
