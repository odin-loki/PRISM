// PRISM conformance task regress/array_init_zero_false.c: expected false (no-div0)
// regression: local arrays: `= {0}` zero-fills every element (C11 6.7.9p21)
int array_init_zero_false(int i) {
    int a[4] = {0};
    a[i & 3] = 5;
    return 100 / a[((i & 3) + 1) & 3];
}
