// PRISM conformance task c23/c23_va_opt_true.c: expected true (no-overflow)
#define ADD1(x, ...) ((x) + 1 __VA_OPT__(+ __VA_ARGS__))
int c23_va_opt_true(short x) { return ADD1(x); }
