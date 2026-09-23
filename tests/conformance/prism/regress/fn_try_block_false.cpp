// PRISM conformance task regress/fn_try_block_false.cpp: expected false (no-div0)
// regression: F10: the reference argument keeps the value the callee stores
static void fn_try_block_set(int &x) try {
    x = 0;
} catch (...) {
}

int fn_try_block_false(int n) {
    int v = 10;
    fn_try_block_set(v);
    return 100 / v + (n & 1);
}
