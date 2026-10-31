// FAILED (CONC-DEADLOCK): C11 cnd_wait with no predicate; cnd_signal can come first.
#include <threads.h>

mtx_t m;
cnd_t cv;

int worker(void *arg) {
    (void)arg;
    mtx_lock(&m);
    cnd_signal(&cv);
    mtx_unlock(&m);
    return 0;
}

int main(void) {
    thrd_t t;
    mtx_init(&m, mtx_plain);
    cnd_init(&cv);
    thrd_create(&t, worker, 0);
    mtx_lock(&m);
    cnd_wait(&cv, &m);
    mtx_unlock(&m);
    thrd_join(t, 0);
    return 0;
}
