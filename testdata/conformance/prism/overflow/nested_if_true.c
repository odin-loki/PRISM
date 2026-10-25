// PRISM conformance task overflow/nested_if_true.c: expected true (no-overflow)
int nested_if_true(int a, int b) {
    if (a > 0) {
        if (b > 0) {
            if (a < 30000 && b < 30000) return a * b;
            return 0;
        }
        return a;
    }
    return b > 0 ? 0 : -1;
}
