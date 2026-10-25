// PRISM conformance task memory/mem_misaligned_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int mem_misaligned_false(void) {
    _Alignas(8) char buf[8] = {0};
    return *(int *)(buf + 1);
}
