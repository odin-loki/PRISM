#include <stdlib.h>

/* p = realloc(p, n) leaks the old block on failure. */
void *grow(void *p, unsigned n) {
    p = realloc(p, n);
    return p;
}
