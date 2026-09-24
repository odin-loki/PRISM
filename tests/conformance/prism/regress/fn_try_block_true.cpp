// PRISM conformance task regress/fn_try_block_true.cpp: expected true (no-div0)
// regression: F10: a callee bmc does not model (here a function-try-block)
// may write an argument bound to a non-const reference
static void fn_try_block_set(int &x) try {
    x = 10;
} catch (...) {
}

int fn_try_block_true(int n) {
    int v = 0;
    fn_try_block_set(v);
    return 100 / v + (n & 1);
}
