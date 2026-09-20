/* FuSeBMC-style goals: implicit else, loop-exit, switch cases. Scalar only. */
int fuse_goals(int x, int k) {
    int n = x;
    if (x > 0)
        n = x;
    while (n > 0)
        n = n - 1;
    switch (k) {
    case 1: return n + 1;
    case 2: return n + 2;
    default: return n;
    }
}
