// PRISM conformance task regress/dangling_else_false.c: expected false (no-div0)
// regression: parser: else binds to the innermost if
int dangling_else_false(int a, int b) {
    int x = 1;
    if (a)
        if (b) x = 2;
        else x = 0;
    return 10 / x;
}
