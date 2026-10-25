/* SCALAR, loop-free. PRISM BMC should PROVED-UNBOUNDED: no UB on any input. */
int abs_ok(int x) {
    if (x < 0) {
        if (x == (-2147483647 - 1)) {
            return 2147483647;
        }
        return -x;
    }
    return x;
}
