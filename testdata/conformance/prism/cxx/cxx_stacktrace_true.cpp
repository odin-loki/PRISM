// PRISM conformance task cxx/cxx_stacktrace_true.cpp: expected true (no-oob)
// current() has size 0 or 1. empty() is that size == 0. [0] is used only
// when size() is 1, which is the trace empty() reports as not empty.
#include <stacktrace>
int cxx_stacktrace_true(int n) {
    std::stacktrace st = std::stacktrace::current();
    if (st.size() == 0) return st.empty() ? n : 1;
    if (st.empty()) return 1;
    if (st.size() != 1) return n;
    if (st[0].native_handle() == nullptr) return 1;
    return n;
}
