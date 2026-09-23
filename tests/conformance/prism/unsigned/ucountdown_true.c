// PRISM conformance task unsigned/ucountdown_true.c: expected true (no-overflow)
unsigned ucountdown_true(unsigned n) {
    unsigned c = 0;
    if (n > 1000) return 0;
    for (unsigned i = n; i > 0; i--) c += i;
    return c;
}
