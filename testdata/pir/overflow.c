/* PIR tasks: signed overflow (true and false variants). */
int add_bad(int a, int b) { return a + b; }
int add_ok(int a) {
    if (a > 1000 || a < -1000)
        return 0;
    return a + 1;
}
unsigned add_unsigned_ok(unsigned a, unsigned b) { return a + b; }
long mul_bad(long a) { return a * 3; }
int neg_bad(int a) { return -a; }
