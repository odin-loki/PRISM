// PRISM conformance task overflow/switch_false.c: expected false (no-overflow)
int switch_false(int op, int x) {
    if (x < -1000 || x > 1000) return 0;
    switch (op) {
    case 0: return x + 1;
    case 1: return x * x * x * x;
    default: return 0;
    }
}
