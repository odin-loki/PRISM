/* PIR tasks: k-induction. A closed step is PROVED-UNBOUNDED; an open step
 * stays BOUNDED (never FAILED from a havoced state). */
int kind_closed(int n) {
    while (n > 0)
        n--;
    return n;
}
unsigned kind_countdown(unsigned n) {
    unsigned s = 0;
    while (n != 0) {
        n--;
        s++;
    }
    return s;
}
int kind_open(int n) {
    int x = 1;
    while (n > 0) {
        x = x + x;
        n--;
    }
    return x;
}
