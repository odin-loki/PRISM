int function_ref_named(void) {
    return 1;
}

int function_ref_bad(void) {
    std::function_ref<int()> r = []{ return 1; };
    return r();
}

int function_ref_ok(void) {
    std::function_ref<int()> r = function_ref_named;
    return r();
}
