// PRISM conformance task c99/c99_pragma_op_true.c: expected true (no-overflow)
#define NOWARN _Pragma("clang diagnostic ignored \"-Wunused\"")
NOWARN
int c99_pragma_op_true(short x) { return x + 1; }
