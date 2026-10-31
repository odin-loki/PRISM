// PRISM conformance task c23/c23_va_opt_false.c: expected false (no-overflow)
#define ADD1(x, ...) ((x) + 1 __VA_OPT__(+ __VA_ARGS__))
int c23_va_opt_false(int x) { return ADD1(x, 0); }
