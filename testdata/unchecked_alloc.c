#include <stdlib.h>

struct node {
    int x;
};

void unchecked_alloc(void) {
    struct node *p;
    p = malloc(8);
    p->x = 1;
}

void checked_alloc(void) {
    struct node *p;
    p = malloc(8);
    if (p == NULL)
        return;
    p->x = 1;
}

void alloc_in_condition(unsigned n) {
    struct node *p;
    if ((p = malloc(n)) == NULL)
        return;
    p->x = 1;
}
