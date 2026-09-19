/* k-induction: closing the step is PROVED-UNBOUNDED.
 * SAT on a havoced step stays BOUNDED — never a FAILED of the original. */

int kinduct_closed(int n) {
    while (n > 0) {
        n = n - 1;
    }
    return n;
}

int kinduct_step_open(int n) {
    int x;
    x = 1;
    while (n > 0) {
        x = x + x;
        n = n - 1;
    }
    return x;
}
