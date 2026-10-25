int int128t_unenc_bad(int n) {
    __int128_t x = n;
    return (int)x;
}

int int128t_unenc_ok(int n) {
    return n;
}
