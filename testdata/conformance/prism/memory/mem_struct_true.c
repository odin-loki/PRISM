// PRISM conformance task memory/mem_struct_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
struct rec {
    int id;
    char tag[4];
    long v;
};
int mem_struct_true(int i) {
    struct rec r = {1, {'a', 'b', 'c', 0}, 2};
    return r.tag[i & 3] + r.id;
}
