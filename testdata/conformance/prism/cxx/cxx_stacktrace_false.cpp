// PRISM conformance task cxx/cxx_stacktrace_false.cpp: expected false (no-oob)
// operator[] on an empty stacktrace (x == 0 leaves it empty). empty() does
// not hand back a frame, so the index is still out of range.
#include <stacktrace>
int cxx_stacktrace_false(int x) {
    std::stacktrace st;
    if (x) return x;
    if (!st.empty()) return x;
    return st[0].native_handle() != nullptr ? 1 : 0;
}
