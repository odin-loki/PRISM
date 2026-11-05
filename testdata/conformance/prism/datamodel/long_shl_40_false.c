// PRISM conformance task datamodel/long_shl_40_false.c: expected false (no-shift-ub) for x86_64-pc-windows-msvc
// long is 32-bit on Windows x64 (LLP64): 1L << 32 is past its width
// The same body is the _true task for the other data model (LP64 vs LLP64).
long long_shl_40_false(int s) {
    if (s < 0 || s > 40) return 0;
    return 1L << s;
}
