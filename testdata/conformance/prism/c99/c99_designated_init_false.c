// PRISM conformance task c99/c99_designated_init_false.c: expected false (no-div0)
struct s { int a, b, c; };
int c99_designated_init_false(int x) {
    struct s v = { .c = 3, .a = 1 };
    return x / v.b;
}
