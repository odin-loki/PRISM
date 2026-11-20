/* PIR tasks: grounded call-site discharge of the automatic harness
 * (--pir-harness --pir-discharge --pir-discharge-closure; docs/HARNESS_GENERATOR.md "Whole-build call-site
 * discharge"). A callee whose harness is discharged at every call site, each checked in a context that is itself
 * unconditional (a caller without a harness, or one whose harness is discharged the same way), assumes nothing and
 * is PROVED. Every other case keeps an assumption:
 *   leaf_bad     one of its two callers passes a NULL pointer
 *   leaf_ext     external linkage: callers outside the unit are not known
 *   leaf_entry   its only caller is an entry function with a pointer parameter (not grounded)
 *   leaf_taken   its address is taken and it is called through the table (an unresolved indirect caller)
 *   leaf_global  its caller passes a global the callee also writes: the harness took its object to be distinct
 *                from every global, and with the alias the division is by zero (ob-origin keeps the assumption) */

struct dev { int id; int regs[3]; };

/* ---- grounded: the only caller has no pointer parameter and passes an initialised local ---- */
static int leaf_ok(struct dev *d) { return d->id ^ d->regs[2]; }
int top_ok(void) {
    struct dev v = {1, {2, 3, 4}};
    return leaf_ok(&v);
}

/* ---- grounded through a chain: mid's own harness is discharged at top_chain ---- */
static int leaf_chain(struct dev *d) { return d->id & d->regs[1]; }
static int mid_chain(struct dev *d) { return leaf_chain(d) | 1; }
int top_chain(void) {
    struct dev v = {5, {6, 7, 8}};
    return mid_chain(&v);
}

/* ---- one unsatisfying caller ---- */
static int leaf_bad(struct dev *d) { return d->regs[0]; }
int top_bad_ok(void) {
    struct dev v = {1, {2, 3, 4}};
    return leaf_bad(&v);
}
int top_bad_null(void) { return leaf_bad(0); }

/* ---- external linkage ---- */
int leaf_ext(struct dev *d) { return d->id | d->regs[0]; }
int top_ext(void) {
    struct dev v = {1, {2, 3, 4}};
    return leaf_ext(&v);
}

/* ---- the only caller is an entry with a pointer parameter: discharged, not grounded ---- */
static int leaf_entry(struct dev *d) { return d->regs[1]; }
int entry_with_param(struct dev *d) { return leaf_entry(d); }

/* ---- address taken, called through a pointer ---- */
static int leaf_taken(struct dev *d) { return d->regs[2]; }
static int (*const taken_tab[])(struct dev *) = {leaf_taken};
int top_taken(int i) {
    struct dev v = {1, {2, 3, 4}};
    return taken_tab[i & 0](&v) + leaf_taken(&v);
}

/* ---- a global argument the callee also writes ---- */
static struct dev the_dev = {1, {2, 3, 4}};
static int leaf_global(struct dev *d) {
    the_dev.id = 1;
    d->id = -1;
    return 10 / (the_dev.id + 1);
}
int top_global(void) { return leaf_global(&the_dev); }
