int mlock2(const void *addr, unsigned long len, unsigned flags);

void mlock2_bad(void) {
    mlock2(0, 0, 0);
}

void mlock2_ok(void) {
    if (mlock2(0, 0, 0) != 0)
        return;
}
