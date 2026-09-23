// PRISM conformance task overflow/char_promote_true.c: expected true (no-overflow)
// char arithmetic happens in int
int char_promote_true(signed char a, signed char b) {
    return a * b + a - b;
}
