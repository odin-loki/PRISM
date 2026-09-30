#include <stddef.h>

struct link {
    struct link *next;
    int v;
};

void chain_null_bad(struct link *a, int v) {
    a->next->v = v;
}

void chain_null_ok(struct link *a, int v) {
    if (a->next == NULL)
        return;
    a->next->v = v;
}
