// TRUE (norace, noassert): sequentially consistent atomic increments.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

atomic_int counter = 0;

void *worker(void *arg) {
    (void)arg;
    atomic_fetch_add(&counter, 1);
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    assert(atomic_load(&counter) == 2);
    return 0;
}
