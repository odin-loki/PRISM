/* SCALAR for-loop with a constant bound. No signed overflow / div0 / OOB.
 * Helix BMC should close the loop: PROVED-UNBOUNDED or PROVED. */
int loop_prove(int x) {
    int i;
    int s;
    s = 0;
    for (i = 0; i < 4; i = i + 1) {
        if (x > 0)
            x = x - 1;
        s = i;
    }
    return s;
}
