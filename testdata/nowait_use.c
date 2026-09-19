#define M_NOWAIT 0x02

struct node {
    int x;
};

void nowait_unchecked(unsigned n) {
    struct node *p;
    p = malloc(n, M_NOWAIT);
    p->x = 1;
}

void nowait_checked(unsigned n) {
    struct node *p;
    p = malloc(n, M_NOWAIT);
    if (p == NULL)
        return;
    p->x = 1;
}
