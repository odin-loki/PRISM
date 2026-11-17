/* PIR tasks: container_of harness (--pir-harness --pir-container-of, docs/HARNESS_GENERATOR.md).
 * A parameter used at a negative constant offset points to a member of a larger object; the
 * harness allocates the containing object (type from the getelementptr at the negative offset)
 * and passes a pointer to the member. */

#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))

struct inner { int a; int b; };
struct outer { long pad; int tag; int regs[4]; struct inner in; };

int outer_tag(struct inner *i) {
    struct outer *o = container_of(i, struct outer, in);
    return o->tag + i->a;
}

/* an unchecked index of the container's array: a violation only under the harness (NEEDS-HARNESS) */
int outer_reg_unchecked(struct inner *i, int k) {
    struct outer *o = container_of(i, struct outer, in);
    return o->regs[k];
}

int outer_reg_ok(struct inner *i, int k) {
    struct outer *o = container_of(i, struct outer, in);
    if (k < 0 || k >= 4) return -1;
    return o->regs[k];
}

/* container_of discharged: the caller's inner member lives in an outer */
static int tag_of(struct inner *i) {
    struct outer *o = container_of(i, struct outer, in);
    return o->tag;
}
int container_user_ok(void) {
    struct outer o = {0, 7, {1, 2, 3, 4}, {1, 2}};
    return tag_of(&o.in);
}
/* ... a standalone struct inner is not a member of an outer */
static int tag_of_bad(struct inner *i) {
    struct outer *o = container_of(i, struct outer, in);
    return o->tag;
}
int container_user_bad(void) {
    struct inner lone = {1, 2};
    return tag_of_bad(&lone);
}
