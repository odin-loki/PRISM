// PRISM conformance task overflow/mixed_sign_true.c: expected true (no-overflow)
// int + unsigned converts to unsigned: wraps, never UB
unsigned mixed_sign_true(int a, unsigned b) {
    return a + b;
}
