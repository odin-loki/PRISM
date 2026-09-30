/* The failure arm only returns: the index keeps the doubled size with the
 * old, smaller list. */
#include <stdlib.h>

struct point {
    long out;
};

struct access {
    int have;
    int size;
    struct point *list;
};

struct access *add_point_leaky(struct access *index)
{
    struct point *next;
    if (index->have == index->size) {
        index->size <<= 1;
        next = reallocarray(index->list, index->size, sizeof(struct point));
        if (next == NULL) {
            return NULL;
        }
        index->list = next;
    }
    return index;
}
