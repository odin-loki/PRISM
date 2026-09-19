void function_null_bad(void) {
    std::function<void()> f;
    f();
}

void function_null_ok(void) {
    std::function<void()> fn;
    if (fn)
        fn();
}
