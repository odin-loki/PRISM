void *sbrk(int incr);

void sbrk_bad(void) {
    sbrk(0);
}

void sbrk_ok(void) {
    if (sbrk(0) == 0)
        return;
}
