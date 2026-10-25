// PRISM conformance task memory/mem_struct_self_copy_false.c: expected false (memsafety)
// regression: refinement finding 6: C's memcpy function forbids an exact
// self-copy (C17 7.24.2.1), unlike the llvm.memcpy of a struct assignment
#include <string.h>

int mem_struct_self_copy_false(int c) {
    int s[4] = {1, 2, 3, 4}, t[4] = {5, 6, 7, 8};
    int *q = c ? s : t;
    memcpy(s, q, sizeof s);
    return s[0];
}
