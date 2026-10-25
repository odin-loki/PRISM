void bitcast_pun_bad(void) {
    int x;
    x = 1;
    int *ip;
    ip = &x;
    float *fp;
    fp = std::bit_cast<float *>(ip);
    (void)fp;
}

int bitcast_int_ok(int n) {
    return n;
}
