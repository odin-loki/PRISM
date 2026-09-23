/* PRISM conformance task virt/virt_fnptr_true.c: expected true (no-overflow) */
static int twice(int v) { return 2 * v; }
static int negate(int v) { return -v; }
int virt_fnptr_true(int x) {
    int (*f)(int) = x > 0 ? negate : twice;
    if (x < -1000000) return 0;
    return f(x);
}
