int atomic_builtin_unenc_bad(int n) {
    __atomic_load();
    return n;
}

int atomic_builtin_ok(int n) {
    return n;
}
