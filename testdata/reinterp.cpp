void reinterp_pun_bad(void) {
    int x = 1;
    int *ip = &x;
    float *fp = reinterpret_cast<float *>(ip);
    *fp = 1.0f;
}

void reinterp_void_ok(void) {
    int x = 1;
    void *vp = reinterpret_cast<void *>(&x);
    (void)vp;
}
