/* PIR tasks: the nested-pointer depth bound of the automatic harness
 * (--pir-harness-depth). A larger depth gives more pointer fields an object,
 * each a named assumption; it never turns a function whose violation needs an
 * unbound pointer into PROVED, and never into PROVED without assumptions. */

struct cell {
    struct cell *next;
    void *cookie;
    int v;
};

/* five links: below depth 5 the last pointer is arbitrary (NEEDS-HARNESS) */
int chain5(struct cell *c) { return c->next->next->next->next->next->v; }

/* the read is past any default object: whatever the depth, the pointer it
 * goes through is bounded by nothing the code states (never a proof) */
int cookie_far(struct cell *c) {
    const int *q = c->next->cookie;
    return q ? q[1000] : 0;
}

/* walks the list as far as it goes: the depth bound always leaves a link */
int walk(struct cell *c) {
    int s = 0;
    while (c) {
        s ^= c->v;
        c = c->next;
    }
    return s;
}
