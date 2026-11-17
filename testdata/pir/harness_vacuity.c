/* PIR tasks: non-vacuity of the harness (--pir-harness --pir-vacuity, docs/HARNESS_GENERATOR.md).
 * An assumption that no execution satisfies makes every check pass trivially; the verdict must not
 * be a proof. assume_abort_if_not is SV-COMP's assume: abort() inside it ends the path. */

extern void abort(void);
void assume_abort_if_not(int c) { if (!c) abort(); }

struct dev { int id; int regs[4]; };

/* a satisfiable assumption: some execution reaches the end */
int vac_ok(struct dev *d) {
    assume_abort_if_not(d->id >= 0 && d->id < 4);
    return d->regs[d->id];
}

/* a contradictory one: no execution does, so the "proof" says nothing */
int vac_contradiction(struct dev *d) {
    assume_abort_if_not(d->id > 5 && d->id < 3);
    return d->regs[d->id];
}

/* a contradictory harness-independent chain on a parameter-free path: nothing to check */
int vac_plain(struct dev *d) { return d->id; }
