// PRISM conformance task c99/c99_variadic_macro_false.c: expected false (no-div0)
#define DIV(a, ...) ((a) / (__VA_ARGS__))
int c99_variadic_macro_false(int x) {
    return DIV(100, x);
}
