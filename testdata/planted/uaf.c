#include <stdlib.h>

struct node {
    int x;
};

void uaf_bad(void) {
    struct node *p;
    p = malloc(sizeof(*p));
    free(p);
    p = NULL;
    p->x = 1;
}

void checked_use(void) {
    struct node *p;
    p = malloc(sizeof(*p));
    if (p == NULL)
        return;
    p->x = 1;
    free(p);
}
