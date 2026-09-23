// PRISM conformance task unsigned/usub_false.c: expected false (no-overflow)
unsigned usub_false(unsigned a, int b) {
    return a - (unsigned)(-b);
}
