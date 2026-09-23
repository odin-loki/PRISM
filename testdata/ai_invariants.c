/* Loop invariant synthesis (roadmap 4.2). Each loop runs past the BMC
 * unwind, and the plain induction step from an arbitrary state is open, so
 * without invariants the verdict is BOUNDED. Template invariants filtered by
 * Houdini close the step and the base case: PROVED-UNBOUNDED. */

/* needs: i >= 0, i <= 1000, s == 2 * i */
int ai_sum_to_n(int n) {
    int i;
    int s;
    s = 0;
    i = 0;
    if (n > 1000) return 0;
    while (i < n) {
        s = s + 2;
        i = i + 1;
    }
    return s;
}

/* needs: i >= 0, i <= 16 (the store a[i] stays in bounds) */
int ai_fill(int k) {
    int a[16];
    int i;
    for (i = 0; i < 16; i = i + 1) {
        a[i] = k;
    }
    return a[3];
}

/* needs: i + j == 100 and i <= 100; the division after the loop is safe
 * only because of the relation between the counters. */
int ai_pair(int n) {
    int i;
    int j;
    i = 0;
    j = 100;
    if (n < 0 || n > 100) return 0;
    while (i < n) {
        i = i + 1;
        j = j - 1;
    }
    return 1000 / (j + i - 99);
}

/* Real overflow for n >= 31 (beyond the unwind): no invariant can prove it,
 * so it must stay BOUNDED. */
int ai_doubling(int n) {
    int x;
    x = 1;
    while (n > 0) {
        x = x + x;
        n = n - 1;
    }
    return x;
}
