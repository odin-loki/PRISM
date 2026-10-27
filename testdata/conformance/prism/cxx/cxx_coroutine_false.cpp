// PRISM conformance task cxx/cxx_coroutine_false.cpp: expected false (memsafety)
// resume() on a default handle (x == 0 leaves it null). The call is a check.
// It does not resume a frame and it does not return a value.
#include <coroutine>
int cxx_coroutine_false(int x) {
    std::coroutine_handle<> h;
    if (x) return x;
    h.resume();
    return 1;
}
