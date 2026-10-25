// PRISM conformance task overflow/short_promote_true.c: expected true (no-overflow)
// short operands promote to int: short+short cannot overflow int
int short_promote_true(short a, short b) {
    return a + b;
}
