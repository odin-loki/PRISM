// PRISM conformance task cxx/cxx_coroutine_true.cpp: expected true (memsafety)
// A default handle stores nullptr. from_address(nullptr) is a null handle.
// Neither address is dereferenced, and no coroutine is resumed.
#include <coroutine>
int cxx_coroutine_true(int) {
    std::coroutine_handle<> h;
    if (h.address() != nullptr) return 1;
    if (std::coroutine_handle<>::from_address(nullptr).address() != nullptr) return 1;
    return 0;
}
