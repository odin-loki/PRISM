/* zlib examples/zran.c:110: the size is doubled before realloc(), but the
 * failure arm frees the whole index and returns, so the grown size dies
 * with it. */
#include <stdlib.h>

struct point {
    long out;
};

struct access {
    int have;
    int size;
    struct point *list;
};

void free_index(struct access *index)
{
    if (index != NULL) {
        free(index->list);
        free(index);
    }
}

struct access *add_point(struct access *index)
{
    struct point *next;
    if (index->have == index->size) {
        index->size <<= 1;
        next = reallocarray(index->list, index->size, sizeof(struct point));
        if (next == NULL) {
            free_index(index);
            return NULL;
        }
        index->list = next;
    }
    return index;
}
