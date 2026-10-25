/* PIR tasks: calls to functions in the same unit are inlined. */
static int helper(int x) { return x + 1; }
int caller_ok(int x) {
    if (x > 100)
        return 0;
    return helper(x);
}
int caller_bad(int x) { return helper(x) * 2; }
