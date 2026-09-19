int minherit(void *addr, unsigned long len, int inherit);

void minherit_bad(void) {
    minherit(0, 0, 0);
}

void minherit_ok(void) {
    if (minherit(0, 0, 0) != 0)
        return;
}
