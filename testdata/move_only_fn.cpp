void move_only_fn_bad(void) {
    std::move_only_function<void()> f;
    f();
}

void move_only_fn_ok(void) {
    std::move_only_function<void()> f;
    if (f)
        f();
}
