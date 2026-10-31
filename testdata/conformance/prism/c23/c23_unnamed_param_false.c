// PRISM conformance task c23/c23_unnamed_param_false.c: expected false (no-div0)
static int pick(int, int b) { return b; }
int c23_unnamed_param_false(int x) {
    return 100 / pick(5, x);
}
