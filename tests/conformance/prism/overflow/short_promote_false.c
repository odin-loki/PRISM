// PRISM conformance task overflow/short_promote_false.c: expected false (no-overflow)
int short_promote_false(short a, short b) {
    int s = a * b;
    return s * 3;
}
