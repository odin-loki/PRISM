/* Whole-build call-site discharge (prism-qa callsite-discharge; docs/HARNESS_GENERATOR.md "Whole-build call-site
 * discharge"): unit a of a two-unit module "demo". Its external functions are called from b.c too. */
struct dev { int id; int regs[3]; };

/* called here and in b.c, every caller passes an initialised local: PROVED over the build (link rule linux) */
int ext_leaf(struct dev *d) { return d->id ^ d->regs[2]; }
/* b.c passes a buffer smaller than the harness: stays an assumption */
int ext_bad(struct dev *d) { return d->id ^ d->regs[2]; }
/* exported: EXPORT_SYMBOL takes its address (__ADDRESSABLE); stays an assumption */
int ext_exported(struct dev *d) { return d->id ^ d->regs[1]; }
static void *const addressable_ext_exported = (void *)&ext_exported;
void *keep_addressable(void) { return addressable_ext_exported; }
/* b.c calls it through a function pointer: an unresolved indirect caller, stays an assumption */
int ext_indirect(struct dev *d) { return d->regs[0] & d->id; }

int a_user(void) {
    struct dev v = {1, {2, 3, 4}};
    return ext_leaf(&v);
}
