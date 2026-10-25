// PRISM conformance task unsigned/ucountdown_false.c: expected false (no-overflow)
unsigned ucountdown_false(unsigned n) {
    int c = 0;
    if (n > 100000) return 0;
    for (unsigned i = n; i > 0; i--) c += (int)i;
    return c;
}
