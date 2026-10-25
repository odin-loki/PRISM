// PRISM conformance task shift/llshl_true.c: expected true (no-shift-ub)
long long llshl_true(long long a, int s) {
    if (a < 0 || a > 255 || s < 0 || s > 40) return 0;
    return a << s;
}
