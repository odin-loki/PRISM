#include <stdlib.h>

struct flex {
    int n;
    char data[];
};

struct item {
    int x;
};

void flex_bad(void) {
    struct flex *p;
    p = malloc(sizeof(struct flex));
}

void flex_star_bad(void) {
    struct flex *p;
    p = malloc(sizeof(*p));
}

void flex_ok(int n) {
    struct flex *p;
    p = malloc(sizeof(struct flex) + (unsigned)n);
}

void flex_plain_ok(void) {
    struct item *p;
    p = malloc(sizeof(struct item));
}
