/* Countdown with requires + decreases variant on the loop counter. */
int countdown(int n) {
    // requires: n >= 0
    // requires: n < 8
    // decreases: i
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}

/* Same loop shape but no requires — must not become PROVED-UNBOUNDED. */
int countdown_open(int n) {
    // decreases: i
    // ensures: result == 0
    int i = n;
    while (i > 0) { i = i - 1; }
    return i;
}
