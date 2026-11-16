// EXPECT: BOUNDED. IRIW with seq_cst accesses: all threads agree on one total
// order, so the disagreeing outcome is forbidden. Source: C11 7.17.3 (single
// total order S over seq_cst operations); herd7 C11 IRIW+scs.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1, r3 = -1, r4 = -1;
void *w1(void *p) { (void)p; atomic_store(&x, 1); return 0; }
void *w2(void *p) { (void)p; atomic_store(&y, 1); return 0; }
void *rd1(void *p) { (void)p; r1 = atomic_load(&x); r2 = atomic_load(&y); return 0; }
void *rd2(void *p) { (void)p; r3 = atomic_load(&y); r4 = atomic_load(&x); return 0; }
int main(void) { pthread_t t0, t1, t2, t3; pthread_create(&t0, 0, w1, 0); pthread_create(&t1, 0, w2, 0);
    pthread_create(&t2, 0, rd1, 0); pthread_create(&t3, 0, rd2, 0);
    pthread_join(t0, 0); pthread_join(t1, 0); pthread_join(t2, 0); pthread_join(t3, 0);
    assert(!(r1 == 1 && r2 == 0 && r3 == 1 && r4 == 0)); return 0; }
