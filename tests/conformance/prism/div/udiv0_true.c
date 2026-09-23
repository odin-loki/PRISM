// PRISM conformance task div/udiv0_true.c: expected true (no-div0)
unsigned udiv0_true(unsigned a, unsigned b) {
    return b ? a / b : 0;
}
