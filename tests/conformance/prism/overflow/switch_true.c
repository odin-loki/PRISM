// PRISM conformance task overflow/switch_true.c: expected true (no-overflow)
int switch_true(int op, int x) {
    if (x < -1000 || x > 1000) return 0;
    switch (op) {
    case 0: return x + 1;
    case 1: return x * x;
    case 2: return x - 7;
    default: return 0;
    }
}
