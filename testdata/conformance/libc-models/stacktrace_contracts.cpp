// std::stacktrace model (src/prism/pir/models/cxx/stacktrace). The pir stage
// puts the model directory first on the include path, so <stacktrace> below
// is the model. empty() is size() == 0 and does not read a frame. The false
// function indexes an empty trace.
#include <stacktrace>

int stacktrace_empty_true(int c) {
    std::stacktrace st;
    if (c < 0) return 0;
    if (!st.empty()) return 1;
    return static_cast<int>(st.size());
}

int stacktrace_current_bound_true(int c) {
    std::stacktrace st = std::stacktrace::current();
    if (st.size() == 0) return st.empty() ? 0 : c;
    if (st.empty()) return c;
    if (st.size() != 1) return c;
    return st[0].native_handle() != nullptr ? 0 : c;
}

int stacktrace_empty_index_false(int c) {
    std::stacktrace st;
    if (c) return c;
    if (!st.empty()) return c;
    return st[0].native_handle() != nullptr ? 1 : 0;
}
