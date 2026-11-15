// EXPECT: BOUNDED. Load buffering, relaxed: r1 == r2 == 1 is forbidden in this
// model. The ISO C11 text allows it, but RC11 forbids it (no po;rf cycles, the
// no-thin-air rule). Source: Lahav et al., PLDI 2017, sec. 3 and fig. 1 (LB);
// the existing view model follows RC11 (docs/CONCURRENCY.md sec. 6).
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1;
void *a(void *p) { (void)p; r1 = atomic_load_explicit(&x, memory_order_relaxed); atomic_store_explicit(&y, 1, memory_order_relaxed); return 0; }
void *b(void *p) { (void)p; r2 = atomic_load_explicit(&y, memory_order_relaxed); atomic_store_explicit(&x, 1, memory_order_relaxed); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r1 == 1 && r2 == 1)); return 0; }
