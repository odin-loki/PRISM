// PRISM conformance task overflow/switch_fallthrough_true.c: expected true (no-overflow)
int switch_fallthrough_true(int op, int x) {
    int r = 0;
    if (x < 0 || x > 100) return 0;
    switch (op) {
    case 0: r += x;
    case 1: r += x;
    case 2: r += x; break;
    default: r = 1;
    }
    return r;
}
