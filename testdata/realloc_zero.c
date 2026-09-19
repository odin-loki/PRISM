#include <stdlib.h>

int realloc_zero_bad(void) {
    char *q;
    char *p = malloc(4);
    q = realloc(p, 0);
    p[0] = 1;
    (void)q;
    return 0;
}

int realloc_zero_ok(void) {
    char *p = malloc(4);
    p = realloc(p, 8);
    if (p)
        p[0] = 1;
    return 0;
}
