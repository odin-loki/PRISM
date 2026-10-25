void popen_unenc_bad(void) {
    popen("x", "r");
}

int popen_unenc_ok(int n) {
    return n;
}
