// BOUNDED: a spinlock from an acquire compare-exchange and a release store
// protects the plain counter (one attempt each: a failed attempt skips).
#include <pthread.h>
#include <stdatomic.h>
atomic_int lk;
int counter;
void *w(void *p) {
    (void)p;
    int z = 0;
    if (atomic_compare_exchange_strong_explicit(&lk, &z, 1, memory_order_acquire, memory_order_relaxed)) {
        counter++;
        atomic_store_explicit(&lk, 0, memory_order_release);
    }
    return 0;
}
int main(void) { pthread_t a, b; pthread_create(&a, 0, w, 0); pthread_create(&b, 0, w, 0);
    pthread_join(a, 0); pthread_join(b, 0); return counter; }
