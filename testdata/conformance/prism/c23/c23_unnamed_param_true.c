// PRISM conformance task c23/c23_unnamed_param_true.c: expected true (no-div0)
static int pick(int, int b) { return b; }
int c23_unnamed_param_true(int x) {
    return 100 / pick(x, 5);
}
