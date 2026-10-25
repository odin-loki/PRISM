// PRISM conformance task regress/array_init_zero_true.c: expected true (no-div0)
// regression: local arrays: `= {0}` zero-fills every element (C11 6.7.9p21)
int array_init_zero_true(int i) {
    int a[4] = {0};
    return 100 / (a[i & 3] + 1);
}
