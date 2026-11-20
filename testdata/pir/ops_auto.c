/* --pir-ops-auto (docs/HARNESS_PHASE2.md "Function-pointer fields"): the unit's own stores into the ops table
 * name the targets of each field; a harness function-pointer field is bound to one of them. */

struct dev_ops {
    int (*read)(int);
    int (*divide)(int);
    int (*unset)(int);   /* never assigned in this unit */
};
struct dev { int id; struct dev_ops ops; };

static int read_safe(int x) { return x & 0x7f; }
static int read_div(int x) { return 100 / x; }

/* the framework's initialisation, in this unit: it names the targets */
void dev_init(struct dev *d) {
    d->ops.read = read_safe;
    d->ops.divide = read_div;
}

/* the field is a function pointer the framework filled in: by default a null call under the harness */
int dev_call_read(struct dev *d, int x) { return d->ops.read(x); }

int dev_call_div_ok(struct dev *d, int x) {
    if (x == 0) return 0;
    return d->ops.divide(x);
}
/* x == 0 reaches the dividing target: a violation under the harness (NEEDS-HARNESS, naming the division) */
int dev_call_div_bad(struct dev *d, int x) { return d->ops.divide(x); }

/* no store in the unit: no targets, the field stays an arbitrary pointer */
int dev_call_unset(struct dev *d, int x) { return d->ops.unset(x); }

/* a static initialiser is a source of targets too */
static const struct dev_ops fixed = {read_safe, read_div, 0};
int fixed_read_ok(int x) { return fixed.read(x); }
