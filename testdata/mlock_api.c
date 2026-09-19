int mlock(const void *addr, unsigned long len);

void mlock_bad(void) {
    mlock(0,0);
}

void mlock_ok(void) {
    if (mlock(0,0)!=0)
        return;
}
