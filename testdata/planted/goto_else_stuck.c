/* bmc goto model (docs/SVCOMP.md "goto"): a jump from a then branch into
 * the start of its else branch, and a self-loop label (`L: goto L;`) that
 * ends every path reaching it. */

/* The division by zero is reachable only through the jump into the else
 * branch (x = 1000 or 1001 there; the if's condition is not evaluated again). */
int goto_else_bad(int x, int y) {
    int r = 0;
    if (x > 0) {
        if (y == 7) goto ELSE;
        r = 1;
    } else {
    ELSE:
        r = 100 / (x / 2 - 500);
    }
    return r;
}

/* Same shape without the violation: the jump never carries x = 1000 or 1001. */
int goto_else_ok(int x, int y) {
    int r = 0;
    if (x > 0) {
        if (y == 7 && x != 1000 && x != 1001) goto ELSE;
        r = 1;
    } else {
    ELSE:
        r = 100 / (x / 2 - 500);
    }
    return r;
}

/* A self-loop label ends the path: the overflow after it is unreachable
 * for i >= 100, and the later goto to the label (into the earlier if
 * body) ends that path too. */
int goto_stuck_ok(int i) {
    if (i >= 100) STUCK: goto STUCK;
    if (i < -100) goto STUCK;
    return i + 2147483547;
}

/* The self-loop stops i >= 102 only: i = 101 overflows. */
int goto_stuck_bad(int i) {
    if (i >= 102) STUCK: goto STUCK;
    return i + 2147483547;
}
