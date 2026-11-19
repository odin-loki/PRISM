/* PIR tasks: container_of through a helper function (--pir-harness --pir-container-of, docs/HARNESS_GENERATOR.md).
 * The kernel writes container_of in a small function of its own (to_foo(hw)); the harness follows the helper's
 * returned pointer back to the parameter (the call's result is the parameter at a constant offset), so the
 * containing object is found as for an inline container_of. */

#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))

struct inner { int a; int b; };
struct outer { long pad; int tag; int regs[4]; struct inner in; };

static struct outer *to_outer(struct inner *i) { return container_of(i, struct outer, in); }

/* two helper levels */
static struct outer *to_outer2(struct inner *i) { return to_outer(i); }

int helper_tag(struct inner *i) {
    struct outer *o = to_outer(i);
    return o->tag ^ i->a;
}

int helper2_tag(struct inner *i) {
    return to_outer2(i)->tag;
}

/* an unchecked index of the container's array: a violation only under the harness (NEEDS-HARNESS) */
int helper_reg_unchecked(struct inner *i, int k) {
    return to_outer(i)->regs[k];
}

int helper_reg_ok(struct inner *i, int k) {
    if (k < 0 || k >= 4) return -1;
    return to_outer(i)->regs[k];
}

/* a helper that returns one of two offsets is not a container_of helper: nothing is derived */
static char *pick(struct inner *i, int c) { return c ? (char *)i - 8 : (char *)i - 16; }
int helper_two_offsets(struct inner *i, int c) {
    return *(int *)pick(i, c);
}

/* an accessor that returns a pointer field (dev_get_drvdata style): the caller's uses of the result are uses of the
 * nested object, so its type comes from them (here a field past the 64-byte default of an untyped object) */
struct priv { char big[100]; int x; };
struct holder { long pad; struct priv *drv; };
static struct priv *get_priv(struct holder *h) { return h->drv; }
int priv_tail(struct holder *h) {
    return get_priv(h)->x;
}

/* the driver-model chain: container_of to the containing device, then its driver data (to_platform_device(dev),
 * platform_get_drvdata(pdev) -> dev_get_drvdata(&pdev->dev)): the field is typed from the caller's uses */
struct priv2 { char big[100]; int x; };
struct dev2 { long id; void *driver_data; };
struct pdev2 { long pad; struct dev2 dev; };
static struct pdev2 *to_pdev2(struct dev2 *d) { return container_of(d, struct pdev2, dev); }
static void *dev2_get_drvdata(struct dev2 *d) { return d->driver_data; }
static void *pdev2_get_drvdata(struct pdev2 *p) { return dev2_get_drvdata(&p->dev); }
int drv_x(struct dev2 *d) {
    struct priv2 *pr = pdev2_get_drvdata(to_pdev2(d));
    return pr->x;
}
