// PRISM conformance task datamodel/long_inc_2p31_false.c: expected false (no-overflow) for x86_64-pc-windows-msvc
// long is 32-bit on Windows x64 (LLP64): 2147483647 + 1 overflows
// The same body is the _true task for the other data model (LP64 vs LLP64).
long long_inc_2p31_false(long a) {
    if (a < 0 || a > 2147483647L) return 0;
    return a + 1;
}
