typedef unsigned long thrd_t;
int thrd_create(thrd_t *t, int (*fn)(void *), void *arg);
int mtx_timedlock(void *m);

int g;

int iso_timed_t1(void *p) { mtx_timedlock(0); g = 1; return 0; }
int iso_timed_t2(void *p) { mtx_timedlock(0); g = 2; return 0; }

void iso_timed_start(void) {
    thrd_t a, b;
    thrd_create(&a, iso_timed_t1, 0);
    thrd_create(&b, iso_timed_t2, 0);
}
