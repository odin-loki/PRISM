// PRISM conformance task regress/dangling_else_true.c: expected true (no-div0)
// regression: parser: else binds to the innermost if
int dangling_else_true(int a, int b) {
    int x = 1;
    if (a)
        if (b) x = 2;
        else x = 5;
    return 10 / x;
}
