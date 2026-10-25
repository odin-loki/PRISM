int promise_bad(void) {
    std::promise<int> p;
    auto f = p.get_future();
    return f.get();
}

int promise_ok(void) {
    std::promise<int> p;
    auto f = p.get_future();
    p.set_value(1);
    return f.get();
}
