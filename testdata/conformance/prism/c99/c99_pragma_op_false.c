// PRISM conformance task c99/c99_pragma_op_false.c: expected false (no-overflow)
#define NOWARN _Pragma("clang diagnostic ignored \"-Wunused\"")
NOWARN
int c99_pragma_op_false(int x) { return x + 1; }
