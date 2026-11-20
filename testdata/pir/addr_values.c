/* --pir-addr-values (docs/HARNESS_PHASE2.md "ptrtoint / inttoptr"): constant ptrtoint / inttoptr expressions and
 * integer-address initialisers. Without the option each is UNENCODED (NEEDS-HARNESS). */

static char tbl[8] = {1, 2, 3, 4, 5, 6, 7, 8};

/* ptrtoint (@tbl) as a value */
unsigned long addr_value(void) { return (unsigned long)&tbl[0]; }

/* two constant addresses of one object: the difference is the offset */
long addr_diff_ok(void) {
    volatile long a = (long)&tbl[3];
    volatile long b = (long)&tbl[0];
    return 100 / (a - b);
}
long addr_diff_bad(void) { /* the difference is 3: this divides by zero */
    volatile long a = (long)&tbl[3];
    volatile long b = (long)&tbl[0];
    return 100 / (a - b - 3);
}

/* an ERR_PTR-style marker: a pointer value that points to no object */
char err_deref_bad(void) {
    char *p = (char *)-22L;
    return *p;
}
char *err_ptr(void) { return (char *)-22L; }

/* a static initialiser with an integer address */
static char *const marker = (char *)-1L;
char marker_deref_bad(void) { return *marker; }
int marker_is_set(void) { return marker == (char *)-1L; }
