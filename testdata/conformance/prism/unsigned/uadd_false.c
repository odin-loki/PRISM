// PRISM conformance task unsigned/uadd_false.c: expected false (no-overflow)
unsigned uadd_false(unsigned a, int b) {
    return a + (unsigned)(b + 1);
}
