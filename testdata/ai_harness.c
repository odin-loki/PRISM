/* Harness drafting (roadmap 4.2). POINTER functions without `requires`
 * end NEEDS-HARNESS (Law 6). A drafted harness lists its assumptions
 * (non-NULL, element count from usage, size range) and the best verdict is
 * PROVED-ASSUMING, never PROVED. */

/* drafted: a != NULL, a has exactly n elements, 1 <= n <= 4 */
int ai_max(int *a, int n) {
    int m;
    int i;
    m = a[0];
    for (i = 1; i < n; i = i + 1) {
        if (a[i] > m)
            m = a[i];
    }
    return m;
}

/* drafted: p != NULL, p has at least 1 element */
int ai_first(int *p) {
    if (p == 0)
        return 0;
    return *p;
}

/* Off by one under the drafted size: a counterexample, but a drafted
 * assumption is not a caller contract, so it stays NEEDS-HARNESS with the
 * counterexample attached rather than a FAILED defect. */
int ai_off_by_one(int *a, int n) {
    int s;
    int i;
    s = 0;
    for (i = 0; i <= n; i = i + 1) {
        s = a[i];
    }
    return s;
}
