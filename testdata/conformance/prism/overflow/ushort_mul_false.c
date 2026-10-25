// PRISM conformance task overflow/ushort_mul_false.c: expected false (no-overflow)
// unsigned short promotes to signed int: 65535*65535 overflows int (classic)
unsigned ushort_mul_false(unsigned short a, unsigned short b) {
    return a * b;
}
