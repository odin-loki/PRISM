// EXPECT: BOUNDED. Read-read coherence (CoRR), relaxed: once a thread has read the
// new value of x it cannot read the older one. Source: C11 5.1.2.4 coherence and
// the modification order (ISO/IEC 9899:2011 7.17.3); Batty et al., POPL 2011,
// sec. 3 (coherence); herd7 C11 CoRR.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x;
int r1 = -1, r2 = -1;
void *w(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); return 0; }
void *rd(void *p) { (void)p; r1 = atomic_load_explicit(&x, memory_order_relaxed); r2 = atomic_load_explicit(&x, memory_order_relaxed); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, w, 0); pthread_create(&b, 0, rd, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r1 == 1 && r2 == 0)); return 0; }
