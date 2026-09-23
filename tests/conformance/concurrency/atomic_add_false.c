// noassert: false. Each access is atomic, but load-then-store is not one
// atomic step: both threads can read 0 and both store 1.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

atomic_int counter = 0;

void *worker(void *arg) {
    (void)arg;
    int v = atomic_load(&counter);
    atomic_store(&counter, v + 1);
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
