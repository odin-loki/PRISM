int coro_unenc_bad(int n) {
    co_await n;
    return n;
}

int coro_unenc_ok(int n) {
    return n;
}
