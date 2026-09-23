/* PIR tasks: k-induction for functions with memory (docs/PIR.md
 * "k-induction with memory"). A loop that only reads memory gets the
 * k-induction step (the memory at every iteration is the prefix's); a loop
 * that writes memory stays BOUNDED. */
static const int table[4] = {1, 2, 3, 4};

int kind_mem_read_closed(unsigned n) {
    int s = 0;
    for (unsigned i = 0; i < n; ++i) s = table[i & 3u] > 2 ? 1 : 0; /* read-only loop */
    return s;
}

int kind_mem_write_bounded(unsigned n) {
    int a[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < n; ++i) a[i & 3u] = 1; /* writes memory: not attempted */
    return a[0];
}

int kind_mem_read_bad(unsigned n) {
    int s = 0;
    for (unsigned i = 0; i < n; ++i) s = table[i & 7u]; /* i == 4: past the end */
    return s;
}
