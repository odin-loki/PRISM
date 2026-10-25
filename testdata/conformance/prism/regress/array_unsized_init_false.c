// PRISM conformance task regress/array_unsized_init_false.c: expected false (no-oob)
// regression: `int a[] = {..}` has exactly as many elements as items
int array_unsized_init_false(int i) {
    int a[] = {1, 2, 3, 4};
    if (i < 0 || i > 4) return 0;
    return a[i];
}
