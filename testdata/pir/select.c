/* PIR tasks: conditional expressions. */
int max_ok(int a, int b) { return a > b ? a : b; }
int sel_div_bad(int a, int b) { return b != 0 ? a / b : 0; }
int abs_bad(int x) { return x < 0 ? -x : x; }
int abs_ok(int x) {
    if (x == -2147483647 - 1)
        return 0;
    return x < 0 ? -x : x;
}
