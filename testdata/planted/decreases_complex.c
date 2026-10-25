/* Dafny-shaped decreases. `*` and calls are not encoded: ERROR, never
   PROVED-ASSUMING. A linear measure is checked: countdown_complex claims
   `n - i`, which grows as i counts down, so it is FAILED. */
int countdown_complex(int n) {
    // requires: n >= 0
    // requires: n < 8
    // decreases: n - i
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}

int countdown_star(int n) {
    // decreases: *
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}

int countdown_abs(int n) {
    // decreases: abs(n)
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}
