int out_ptr_bad(void) {
    std::unique_ptr<int> p;
    foo(std::out_ptr(p));
    return *p;
}

int out_ptr_ok(void) {
    std::unique_ptr<int> p;
    foo(std::out_ptr(p));
    if (!p)
        return 0;
    return *p;
}
