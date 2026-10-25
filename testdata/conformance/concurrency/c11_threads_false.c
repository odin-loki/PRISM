// norace: false. C11 <threads.h>: the second thread skips the mutex.
#include <threads.h>

int counter = 0;
mtx_t m;

int locked(void *arg) {
    (void)arg;
    mtx_lock(&m);
    counter = counter + 1;
    mtx_unlock(&m);
    return 0;
}

int unlocked(void *arg) {
    (void)arg;
    counter = counter + 1;
    return 0;
}

int main(void) {
    thrd_t a, b;
    mtx_init(&m, mtx_plain);
    thrd_create(&a, locked, 0);
    thrd_create(&b, unlocked, 0);
    thrd_join(a, 0);
    thrd_join(b, 0);
    return 0;
}
