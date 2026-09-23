// PRISM conformance task shift/ushl_false.c: expected false (no-shift-ub)
unsigned ushl_false(unsigned a, unsigned s) {
    if (s > 32) return 0;
    return a << s;
}
