// PRISM conformance task shift/ushl_true.c: expected true (no-shift-ub)
unsigned ushl_true(unsigned a, unsigned s) {
    return a << (s & 31u);
}
