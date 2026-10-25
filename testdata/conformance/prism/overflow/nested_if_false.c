// PRISM conformance task overflow/nested_if_false.c: expected false (no-overflow)
int nested_if_false(int a, int b) {
    if (a > 0) {
        if (b > 0) {
            if (a < 70000) return a * b;
            return 0;
        }
        return a;
    }
    return 0;
}
