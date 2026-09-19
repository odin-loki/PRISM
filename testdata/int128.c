int int128_unenc_bad(int n) {
    __int128 x = n;
    return (int)x;
}

int int128_ok(int n) {
    return n;
}
