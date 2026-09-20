int dinit_unenc_bad(int n) {
    int a[2] = {[1] = n};
    return a[0];
}

int dinit_unenc_ok(int n) {
    return n;
}
