// PRISM conformance task c11/c11_alignof_false.c: expected false (no-overflow)
#include <stdalign.h>
int c11_alignof_false(int x) { return x * (int)_Alignof(double); }
