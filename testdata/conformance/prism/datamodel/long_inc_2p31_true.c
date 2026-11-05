// PRISM conformance task datamodel/long_inc_2p31_true.c: expected true (no-overflow) for x86_64-unknown-linux-gnu
// a + 1 for a <= 2^31 - 1 fits a 64-bit long (LP64)
// The same body is the _false task for the other data model (LP64 vs LLP64).
long long_inc_2p31_true(long a) {
    if (a < 0 || a > 2147483647L) return 0;
    return a + 1;
}
