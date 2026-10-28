// std::coroutine_handle<void> model (src/prism/pir/models/cxx/coroutine). The
// pir stage puts the model directory first on the include path, so
// <coroutine> below is the model. A default handle stores nullptr.
// resume, destroy, and done on that handle are FUNC-CONTRACT checks. This
// is not a proof that a coroutine runs. operator== is address() ==
// address(). Two default handles compare equal. A default handle is not
// equal to from_address of a non-null pointer. That comparison is not a
// proof that a coroutine ran. An await expression, a promise member type,
// and a no-op handle are not part of the model.
#include <coroutine>

int coroutine_null_address_true(int) {
    std::coroutine_handle<> h;
    return h.address() == nullptr ? 0 : 1;
}

int coroutine_from_address_true(int) {
    const std::coroutine_handle<> h = std::coroutine_handle<>::from_address(nullptr);
    return h.address() == nullptr ? 0 : 1;
}

int coroutine_resume_null_false(int c) {
    std::coroutine_handle<> h;
    if (c) return c;
    h.resume();
    return 1;
}

int coroutine_destroy_null_false(int c) {
    std::coroutine_handle<> h;
    if (c) return c;
    h.destroy();
    return 1;
}

int coroutine_done_null_false(int c) {
    std::coroutine_handle<> h;
    if (c) return c;
    if (h.done()) return 1;
    return 0;
}

int coroutine_equal_defaults_true(int) {
    std::coroutine_handle<> a;
    std::coroutine_handle<> b;
    if (!(a == b)) return 1;
    if (a != b) return 1;
    if (!(a == std::coroutine_handle<>::from_address(nullptr))) return 1;
    return 0;
}

int coroutine_equal_address_true(int) {
    int object = 0;
    std::coroutine_handle<> a;
    const std::coroutine_handle<> live = std::coroutine_handle<>::from_address(&object);
    const std::coroutine_handle<> again = std::coroutine_handle<>::from_address(&object);
    if (live.address() != &object) return 1;
    if (a == live) return 1;
    if (!(a != live)) return 1;
    if (!(live == again)) return 1;
    if (live != again) return 1;
    return 0;
}
