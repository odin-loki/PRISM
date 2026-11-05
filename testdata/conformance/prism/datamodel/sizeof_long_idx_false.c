// PRISM conformance task datamodel/sizeof_long_idx_false.c: expected false (no-oob) for x86_64-unknown-linux-gnu
// sizeof(long) is 8 on LP64: a[8] is past the end of int a[6]
// The same body is the _true task for the other data model (LP64 vs LLP64).
int sizeof_long_idx_false(int i) {
    int a[6] = {0};
    (void)i;
    return a[sizeof(long)];
}
