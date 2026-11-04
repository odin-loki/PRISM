// BOUNDED: a plain initialisation of an atomic before pthread_create, then
// relaxed accesses: create orders the plain store, so no race.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x;
void *t1(void *p) { (void)p; atomic_store_explicit(&x, 2, memory_order_relaxed); return 0; }
void *t2(void *p) { (void)p; int v = atomic_load_explicit(&x, memory_order_relaxed); assert(v == 1 || v == 2); return 0; }
int main(void) { atomic_init(&x, 1); pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
