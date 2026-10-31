// PRISM conformance task c99/c99_variadic_macro_true.c: expected true (no-div0)
#define DIV(a, ...) ((a) / (__VA_ARGS__))
int c99_variadic_macro_true(int x) {
    return DIV(100, (x | 1));
}
