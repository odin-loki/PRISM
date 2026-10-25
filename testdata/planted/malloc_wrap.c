#include <stdlib.h>

struct item {
    int x;
};

void wrap_alloc_bad(unsigned n) {
    struct item *p;
    p = malloc(n * sizeof(*p));
}

void calloc_ok(unsigned n) {
    struct item *p;
    p = calloc(n, sizeof(*p));
    p = malloc(sizeof(*p));
}
