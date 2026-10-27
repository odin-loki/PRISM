// std::coroutine_handle<void> model (src/prism/pir/models/cxx/coroutine). The
// pir stage puts the model directory first on the include path, so
// <coroutine> below is the model. A default handle stores nullptr.
// resume, destroy, and done on that handle are FUNC-CONTRACT checks. This
// is not a proof that a coroutine runs. An await expression, a promise
// member type, and a no-op handle are not part of the model.
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
