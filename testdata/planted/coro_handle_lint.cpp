void coro_h_bad(void) {
    std::coroutine_handle<> h;
    h.resume();
}

void coro_h_ok(void) {
    std::coroutine_handle<> h;
    if (h.done()) return;
    h.resume();
}
