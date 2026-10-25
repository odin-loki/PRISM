// PRISM conformance task memory/mem_struct_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
struct rec {
    int id;
    char tag[4];
    long v;
};
int mem_struct_false(int i) {
    struct rec r = {1, {'a', 'b', 'c', 0}, 2};
    if (i < 0 || i > 4) return 0;
    return r.tag[i] + r.id;
}
