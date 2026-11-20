/* Whole-build call-site discharge: unit b of the module "demo" (see a.c). */
struct dev { int id; int regs[3]; };
int ext_leaf(struct dev *d);
int ext_bad(struct dev *d);
int ext_exported(struct dev *d);
int ext_indirect(struct dev *d);

int b_user(void) {
    struct dev v = {5, {6, 7, 8}};
    return ext_leaf(&v) & ext_exported(&v);
}
int b_bad(void) {
    char buf[8] = {0};
    return ext_bad((struct dev *)buf);
}
int (*b_hook)(struct dev *) = ext_indirect;
int b_indirect(void) {
    struct dev v = {5, {6, 7, 8}};
    return b_hook(&v) + ext_indirect(&v);
}
