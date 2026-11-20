/* PIR tasks: automatic pointer harness (--pir-harness, docs/PIR.md
 * "Automatic pointer harness"). The pointer parameter is bound to an object
 * of the pointee struct, fields arbitrary, nested pointers to --pir-harness-depth
 * levels; every verdict is PROVED-ASSUMING / BOUNDED / NEEDS-HARNESS, never
 * PROVED. Without --pir-harness every function here is NEEDS-HARNESS (Law 6). */

struct inner { int a; int b; };

struct dev {
    int id;
    int regs[4];
    struct inner *in;
    long *stats;
    unsigned flags;
};

struct node { int v; struct node *next; };

int dev_id(struct dev *d) { return d->id; }

int dev_reg_ok(struct dev *d, int i) {
    if (i < 0 || i >= 4) return -1;
    return d->regs[i];
}

/* the index is unchecked: a violation only under the harness (NEEDS-HARNESS) */
int dev_reg_unchecked(struct dev *d, int i) { return d->regs[i]; }

unsigned dev_flags(struct dev *d) { return d->flags >> (d->id & 31); }

/* one nested level */
int dev_inner(struct dev *d) { return d->in->a ^ d->in->b; }

static int helper_a(struct inner *p) { return p->a; }
int dev_via_helper(struct dev *d) { return helper_a(d->in); }

/* a scalar pointee reached through a field: default element count */
int dev_stat0(struct dev *d) { return (int)d->stats[0]; }

/* arbitrary field values: the division fails when id is 0 (NEEDS-HARNESS) */
int dev_div(struct dev *d) { return 100 / d->id; }

/* three links: depth 2 (the default) leaves the third pointer arbitrary */
int list_third(struct node *n) { return n->next->next->next->v; }
int list_second(struct node *n) { return n->next->next->v; }
