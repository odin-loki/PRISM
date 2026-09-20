int atqexit_unenc_bad(int n) {
    at_quick_exit();
    return n;
}

int atqexit_unenc_ok(int n) {
    return n;
}
