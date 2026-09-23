// PRISM conformance task memory/mem_misaligned_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_misaligned_true(void) {
    _Alignas(8) char buf[8] = {0};
    return *(int *)(buf + 4);
}
