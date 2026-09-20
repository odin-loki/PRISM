int ccast_unenc_bad(int n) {
    const int x = n;
    *const_cast<int*>(&x) = 2;
    return x;
}

int ccast_unenc_ok(int n) {
    return n;
}
