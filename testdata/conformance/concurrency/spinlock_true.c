// noassert / norace: true. A compare-and-swap spin lock protects the counter.
#include <assert.h>
#include <pthread.h>

int lock = 0;
int counter = 0;

void acquire(void) {
    while (!__sync_bool_compare_and_swap(&lock, 0, 1)) {
    }
}

void release(void) { __atomic_store_n(&lock, 0, __ATOMIC_SEQ_CST); }

void *worker(void *arg) {
    (void)arg;
    acquire();
    counter = counter + 1;
    release();
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    assert(counter == 2);
    return 0;
}
