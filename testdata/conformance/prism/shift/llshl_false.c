// PRISM conformance task shift/llshl_false.c: expected false (no-shift-ub)
long long llshl_false(long long a, int s) {
    if (a < 0 || a > 255 || s < 0) return 0;
    return a << s;
}
