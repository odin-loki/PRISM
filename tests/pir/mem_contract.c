/* PIR tasks: pointer parameters (Law 6). Without a precondition a pointer
 * parameter is NEEDS-HARNESS; `// requires: \valid(...)` gives the object
 * size and the verdict is PROVED-ASSUMING with the assumptions listed. */

int no_contract(int *p) { return p[0]; }

// requires: \valid(p + (0..3))
long first_last_ok(int *p) { return (long)p[0] + p[3]; }

// requires: \valid(p + (0..3))
int past_end_bad(int *p) { return p[4]; }

/*@ requires \valid(p + (0..n-1)); */
int count_pos_ok(const int *p, int n) {
    int s = 0;
    if (n > 6) return 0;
    for (int i = 0; i < n; i++) s += (p[i] > 0);
    return s;
}

// requires: \valid(p + (0..n-1))
int off_by_one_bad(int *p, int n) {
    if (n < 0 || n > 6) return 0;
    for (int i = 0; i <= n; i++) p[i] = 0; /* i == n */
    return 0;
}

// requires: \valid_read(s + (0..7))
int readonly_write_bad(char *s) {
    s[0] = 'x'; /* \valid_read only */
    return 0;
}

/* No precondition: NEEDS-HARNESS by default. With --pir-drafts the
 * deterministic template harness draft (ai::draft_harness) gives "a points to
 * exactly n int elements" and the size range "1 <= n <= 4"; both are listed
 * as assumptions (PROVED-ASSUMING). */
int draft_count(int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += (a[i] > 0);
    return s;
}
