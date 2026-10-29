// PRISM conformance task cxx/cxx_coroutine_equal_false.cpp: expected false (memsafety)
// two handles from the same address compare equal; the address is then read one past a two-int object
#include <coroutine>
int cxx_coroutine_equal_false(int n) {
    int object[2] = {0, 1};
    const std::coroutine_handle<> a = std::coroutine_handle<>::from_address(&object[0]);
    const std::coroutine_handle<> b = std::coroutine_handle<>::from_address(&object[0]);
    if (a != b) return 1;
    if (n < 0 || n > 2) return 0;
    return static_cast<int*>(a.address())[n];
}
