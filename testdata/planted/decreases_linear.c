/* Linear decreases measures (identifiers, constants, binary + / -).
   Each one is checked, not trusted: a valid measure is PROVED-ASSUMING,
   a measure that grows or overflows is FAILED. */
int countdown_lin(int n) {
    // requires: n >= 0
    // requires: n < 8
    // decreases: i - 0
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}

int countup_gap(int n) {
    // requires: n >= 0
    // requires: n < 8
    // decreases: n - k
    // ensures: result == n
    int k = 0;
    while (k < n) { k = k + 1; }
    return k;
}

int countdown_ovf(int n) {
    // requires: n >= 0
    // requires: n < 8
    // decreases: i + 999999999 + 999999999 + 999999999
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}
