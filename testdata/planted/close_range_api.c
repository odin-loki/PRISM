int close_range(unsigned first, unsigned last, int flags);

void close_range_bad(void) {
    close_range(0,0,0);
}

void close_range_ok(void) {
    if (close_range(0,0,0)!=0)
        return;
}
