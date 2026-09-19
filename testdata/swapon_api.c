int swapon(const char *path, int flags);

void swapon_bad(void) {
    swapon("x", 0);
}

void swapon_ok(void) {
    if (swapon("x", 0)!=-1)
        return;
}
