#include <stdlib.h>

struct table {
    int *idx;
    int size;
};

/* Capacity committed before the allocation that is supposed to earn it. */
int grow_capacity_first(struct table *tstate) {
    int *tmp;
    tstate->size *= 2;
    tmp = realloc(tstate->idx, (unsigned)tstate->size * sizeof(*tmp));
    if (tmp == NULL)
        return -1;
    tstate->idx = tmp;
    return 0;
}
