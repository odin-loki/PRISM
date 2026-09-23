/* PIR tasks: division by zero and INT_MIN / -1. */
int div_bad(int a, int b) { return a / b; }
int div_ok(int a, int b) {
    if (b == 0 || (a == -2147483647 - 1 && b == -1))
        return 0;
    return a / b;
}
int mod_min_bad(int a) {
    if (a == 0)
        return 0;
    return (-2147483647 - 1) % a;
}
unsigned urem_bad(unsigned a, unsigned b) { return a % b; }
