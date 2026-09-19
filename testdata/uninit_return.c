int uninit_ret_bad(void) {
    int x;
    return x;
}

int uninit_ret_ok(void) {
    int x;
    x = 1;
    return x;
}
