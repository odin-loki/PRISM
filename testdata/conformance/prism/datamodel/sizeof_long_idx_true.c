// PRISM conformance task datamodel/sizeof_long_idx_true.c: expected true (no-oob) for x86_64-pc-windows-msvc
// sizeof(long) is 4 on Windows x64 (LLP64): a[4] is inside int a[6]
// The same body is the _false task for the other data model (LP64 vs LLP64).
int sizeof_long_idx_true(int i) {
    int a[6] = {0};
    (void)i;
    return a[sizeof(long)];
}
