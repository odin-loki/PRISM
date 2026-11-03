// PRISM conformance task c11/c11_alignof_true.c: expected true (no-overflow)
#include <stdalign.h>
int c11_alignof_true(short x) { return x * (int)alignof(double); }
