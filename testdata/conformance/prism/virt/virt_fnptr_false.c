/* PRISM conformance task virt/virt_fnptr_false.c: expected false (no-overflow) */
static int twice(int v) { return 2 * v; }
static int negate(int v) { return -v; }
int virt_fnptr_false(int x) {
    int (*f)(int) = x > 0 ? twice : negate;
    return f(x); /* twice overflows for x > INT_MAX / 2 */
}
