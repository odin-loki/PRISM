// BOUNDED: relaxed flag with a release fence before the store and an acquire
// fence after the load (fence-fence synchronisation): data is 42, no race.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
int data;
atomic_int flag;
void *producer(void *p) {
    (void)p;
    data = 42;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&flag, 1, memory_order_relaxed);
    return 0;
}
void *consumer(void *p) {
    (void)p;
    if (atomic_load_explicit(&flag, memory_order_relaxed) == 1) {
        atomic_thread_fence(memory_order_acquire);
        assert(data == 42);
    }
    return 0;
}
int main(void) { pthread_t a, b; pthread_create(&a, 0, producer, 0); pthread_create(&b, 0, consumer, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
