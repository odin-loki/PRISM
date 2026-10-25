// PRISM conformance task unsigned/uneg_false.c: expected false (no-overflow)
// (int)0x80000000 is INT_MIN on two's complement targets; negation overflows
int uneg_false(unsigned a) {
    int x = (int)(a & 0x80000000u) ;
    return -x;
}
