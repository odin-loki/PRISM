/* Dafny-shaped decreases that Helix will not encode.
   Compound measures are ERROR, never PROVED-ASSUMING. */
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
