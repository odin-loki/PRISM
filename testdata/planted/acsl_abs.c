/*@ requires x > -2147483647;
    ensures \result >= 0;
 */
int acsl_abs(int x) {
    if (x < 0) return -x;
    return x;
}

/* Must not inherit acsl_abs's requires. */
int acsl_plain(int x) {
    return x;
}
