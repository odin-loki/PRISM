/* PIR tasks: assume-guarantee discharge of the automatic harness
 * (--pir-harness --pir-discharge, docs/HARNESS_GENERATOR.md). The harness of a static,
 * non-address-taken, non-recursive function is a proof obligation at every call site of the
 * unit. Discharged: the callee's result no longer carries the invented "(auto harness)"
 * assumption (it stays PROVED-ASSUMING: it rests on the entry functions' assumptions).
 * The *_ok callers pass what the harness assumes; each *_bad caller breaks one part of it.
 * (The initialisation obligation covers every byte of the object, padding included: the structs
 * here have none.) */

struct inner { int a; int b; };
struct dev { int id; int regs[3]; struct inner *in; };

/* ---- discharged: the only caller passes a valid, initialised object ---- */
static int get_id(struct dev *d) { return d->id; }
int user_ok(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n};
    return get_id(&v);
}

/* ---- nested pointer: the caller's field points to a valid object ---- */
static int get_in(struct dev *d) { return d->in->a & d->in->b; }
int nested_ok(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n};
    return get_in(&v);
}
/* ... a NULL field is not what the harness assumed */
static int get_in_null(struct dev *d) { return d->in->a; }
int nested_bad(void) {
    struct dev v = {3, {1, 2, 3}, 0};
    return get_in_null(&v);
}

/* ---- NULL argument ---- */
static int null_callee(struct dev *d) { return d->id; }
int null_bad(void) { return null_callee(0); }

/* ---- too small an object (a cast buffer) ---- */
static int small_callee(struct dev *d) { return d->id ^ d->regs[2]; }
int small_bad(void) {
    char buf[8] = {0};
    return small_callee((struct dev *)buf);
}

/* ---- uninitialised bytes the callee reads ---- */
static int uninit_callee(struct dev *d) { return d->regs[2]; }
int uninit_bad(void) {
    struct inner n = {1, 2};
    struct dev v;
    v.id = 1;
    v.in = &n;
    return uninit_callee(&v);
}

/* ---- two parameters: the harness assumes two distinct objects ---- */
static int two_callee(struct dev *a, struct dev *b) { return a->id & b->id; }
int two_ok(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n}, w = {5, {1, 2, 3}, &n};
    return two_callee(&v, &w);
}
int two_alias_bad(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n};
    return two_callee(&v, &v);
}

/* ---- the entry assumption stays: exported, address-taken (a recursive function is not encodable: UNENCODED) ---- */
int exported_callee(struct dev *d) { return d->id; }
int exported_user(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n};
    return exported_callee(&v);
}

static int taken_callee(struct dev *d) { return d->id; }
int (*const taken_table[])(struct dev *) = {taken_callee};
int taken_user(void) {
    struct inner n = {1, 2};
    struct dev v = {3, {1, 2, 3}, &n};
    return taken_callee(&v);
}
