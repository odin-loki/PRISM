/* PIR tasks: --pir-field-invariants (field_mine.cpp, stage_mem.cpp): only the rows a proof needs stay in its
 * assumptions. */
struct fidev { void *buf; int div; int other; };

int div_by_field(struct fidev *d) {
    return 100 / d->div;
}

/* no row makes this one safe: the rows are dropped or not, it is never proved */
int div_by_other(struct fidev *d) {
    return 100 / (d->other - 1);
}
