// NEEDS-HARNESS: relaxed atomics are not encoded yet (SC only).
#include <pthread.h>
#include <stdatomic.h>

atomic_int flag = 0;

void *worker(void *arg) {
    (void)arg;
    atomic_store_explicit(&flag, 1, memory_order_relaxed);
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_join(t, 0);
    return atomic_load_explicit(&flag, memory_order_relaxed);
}
