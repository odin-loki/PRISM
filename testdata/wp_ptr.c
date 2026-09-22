/* Frama-C WP of a pointer postcondition needs a validity model.
   PRISM does not invent a buffer: NEEDS-HARNESS, never unguarded BMC. */
struct wp_cell { int x; };

int wp_ptr_get(struct wp_cell *p) {
    // requires: p != 0
    // ensures: result >= 0
    return p->x;
}

/* ACSL-only POINTER: RapidCheck must NEEDS-HARNESS, not skip. */
/*@ ensures \result >= 0; */
int wp_acsl_ptr(struct wp_cell *p) {
    return p->x;
}
