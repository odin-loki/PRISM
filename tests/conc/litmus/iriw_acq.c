// EXPECT: FAILED. IRIW with acquire loads and relaxed stores: the two readers may
// disagree on the order of the independent writes (no multi-copy atomicity under
// release/acquire). Source: Batty et al., POPL 2011, sec. 4 (IRIW); Lahav et al.,
// PLDI 2017, sec. 2; herd7 C11 IRIW+acqs.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1, r3 = -1, r4 = -1;
void *w1(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); return 0; }
void *w2(void *p) { (void)p; atomic_store_explicit(&y, 1, memory_order_relaxed); return 0; }
void *rd1(void *p) { (void)p; r1 = atomic_load_explicit(&x, memory_order_acquire); r2 = atomic_load_explicit(&y, memory_order_acquire); return 0; }
void *rd2(void *p) { (void)p; r3 = atomic_load_explicit(&y, memory_order_acquire); r4 = atomic_load_explicit(&x, memory_order_acquire); return 0; }
int main(void) { pthread_t t0, t1, t2, t3; pthread_create(&t0, 0, w1, 0); pthread_create(&t1, 0, w2, 0);
    pthread_create(&t2, 0, rd1, 0); pthread_create(&t3, 0, rd2, 0);
    pthread_join(t0, 0); pthread_join(t1, 0); pthread_join(t2, 0); pthread_join(t3, 0);
    assert(!(r1 == 1 && r2 == 0 && r3 == 1 && r4 == 0)); return 0; }
