// PRISM conformance task datamodel/long_shl_40_true.c: expected true (no-shift-ub) for x86_64-unknown-linux-gnu
// 1L << s for s <= 40 is in range for a 64-bit long (LP64)
// The same body is the _false task for the other data model (LP64 vs LLP64).
long long_shl_40_true(int s) {
    if (s < 0 || s > 40) return 0;
    return 1L << s;
}
