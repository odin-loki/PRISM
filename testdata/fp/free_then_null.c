/* Correct: the pointer is nulled after free, so the later free is free(NULL)
 * and the later test reads a NULL pointer, not freed memory. */
#include <stdlib.h>

struct conn {
    char *buf;
};

void conn_reset(struct conn *c)
{
    free(c->buf);
    c->buf = NULL;
}

int reopen(int again)
{
    char *p = malloc(8);
    if (!p)
        return -1;
    p[0] = 1;
    free(p);
    p = NULL;
    if (again) {
        p = malloc(8);
        if (!p)
            return -1;
        p[0] = 2;
    }
    free(p);
    return 0;
}
