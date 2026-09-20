typedef unsigned long thrd_t;
int thrd_create(thrd_t *t, int (*fn)(void *), void *arg);
int mtx_lock(void *m);

int g;
int h;

int iso_thrd_solo(void *p) { g = 1; return 0; }
int iso_thrd_locked_a(void *p) { mtx_lock(0); h = 1; return 0; }
int iso_thrd_locked_b(void *p) { mtx_lock(0); h = 2; return 0; }

void iso_thrd_solo_start(void) {
    thrd_t a;
    thrd_create(&a, iso_thrd_solo, 0);
}

void iso_thrd_locked_start(void) {
    thrd_t a, b;
    thrd_create(&a, iso_thrd_locked_a, 0);
    thrd_create(&b, iso_thrd_locked_b, 0);
}
