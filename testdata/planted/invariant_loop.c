/* Loop with requires + loop invariant; proof is PROVED-ASSUMING. */
int sum_inv(int n) {
    // requires: n >= 0
    // requires: n < 8
    // invariant: s >= 0
    // ensures: result >= 0
    int i;
    int s;
    s = 0;
    i = 0;
    while (i < n) {
        s = s + 1;
        i = i + 1;
    }
    return s;
}
