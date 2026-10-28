// PRISM conformance task cxx/cxx_coroutine_equal_true.cpp: expected true (memsafety)
// Equality is address() == address(). Two default handles compare equal.
// A default handle is not equal to from_address of a non-null pointer.
// The addresses are not dereferenced. This is not a proof that a coroutine ran.
#include <coroutine>
int cxx_coroutine_equal_true(int) {
    std::coroutine_handle<> a;
    std::coroutine_handle<> b;
    if (!(a == b)) return 1;
    if (a != b) return 1;
    int object = 0;
    const std::coroutine_handle<> live = std::coroutine_handle<>::from_address(&object);
    if (live.address() != &object) return 1;
    if (a == live) return 1;
    if (!(a != live)) return 1;
    const std::coroutine_handle<> again = std::coroutine_handle<>::from_address(&object);
    if (!(live == again)) return 1;
    if (live != again) return 1;
    return 0;
}
