// PRISM conformance task overflow/switch_fallthrough_false.c: expected false (no-overflow)
int switch_fallthrough_false(int op, int x) {
    int r = 0;
    if (x < 0) return 0;
    switch (op) {
    case 0: r += x;
    case 1: r += x; break;
    default: r = 1;
    }
    return r;
}
