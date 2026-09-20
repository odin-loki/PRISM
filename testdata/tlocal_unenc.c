int tlocal_unenc_bad(int n) {
    thread_local int x = n;
    return x;
}

int tlocal_unenc_ok(int n) {
    return n;
}
