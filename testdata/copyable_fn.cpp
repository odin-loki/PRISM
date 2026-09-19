void copyable_fn_bad(void) {
    std::copyable_function<void()> f;
    f();
}

void copyable_fn_ok(void) {
    std::copyable_function<void()> f;
    if (f)
        f();
}
