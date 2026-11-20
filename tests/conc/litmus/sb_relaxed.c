// EXPECT: FAILED. Store buffering, all relaxed: r1 == r2 == 0 is allowed.
// Source: C11 (ISO/IEC 9899:2011 7.17.3, 5.1.2.4) and the SB test of herd7's
// C11 catalogue (Batty et al., POPL 2011, sec. 2; Lahav et al., PLDI 2017, fig. 1).
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1;
void *a(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); r1 = atomic_load_explicit(&y, memory_order_relaxed); return 0; }
void *b(void *p) { (void)p; atomic_store_explicit(&y, 1, memory_order_relaxed); r2 = atomic_load_explicit(&x, memory_order_relaxed); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r1 == 0 && r2 == 0)); return 0; }
