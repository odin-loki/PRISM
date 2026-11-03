#define M_NOWAIT 0x02
void *malloc(unsigned long size, int flags); /* kernel-style malloc(size, flags) */
struct node {
    int x;
};
#define NULL ((void *)0)
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
