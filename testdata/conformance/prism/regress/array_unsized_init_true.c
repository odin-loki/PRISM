// PRISM conformance task regress/array_unsized_init_true.c: expected true (no-oob)
// regression: `int a[] = {..}` has exactly as many elements as items
int array_unsized_init_true(int i) {
    int a[] = {1, 2, 3, 4};
    if (i < 0 || i >= 4) return 0;
    return a[i];
}
