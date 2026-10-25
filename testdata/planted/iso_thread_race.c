typedef unsigned long thrd_t;
int thrd_create(thrd_t *t, int (*fn)(void *), void *arg);

int g;

int iso_thrd_t1(void *p) { g = 1; return 0; }
int iso_thrd_t2(void *p) { g = 2; return 0; }

void iso_thrd_start(void) {
    thrd_t a, b;
    thrd_create(&a, iso_thrd_t1, 0);
    thrd_create(&b, iso_thrd_t2, 0);
}
