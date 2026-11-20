// EXPECT: BOUNDED. Write-write coherence (CoWW) seen by a reader: a thread that
// writes x = 1 then x = 2 is observed in that order by any reader, so reading 2
// then 1 is forbidden. Source: C11 5.1.2.4 and 7.17.3 (the modification order is
// consistent with sequenced-before); herd7 C11 CoWW / CoRR2.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x;
int r1 = -1, r2 = -1;
void *w(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); atomic_store_explicit(&x, 2, memory_order_relaxed); return 0; }
void *rd(void *p) { (void)p; r1 = atomic_load_explicit(&x, memory_order_relaxed); r2 = atomic_load_explicit(&x, memory_order_relaxed); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, w, 0); pthread_create(&b, 0, rd, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r1 == 2 && r2 == 1)); return 0; }
