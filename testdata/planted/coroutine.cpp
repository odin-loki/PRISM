int coro_await_bad(int n) {
    co_await n;
    return n;
}

int coro_ok(int n) {
    return n;
}
