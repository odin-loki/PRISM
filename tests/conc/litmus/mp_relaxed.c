// EXPECT: FAILED. Message passing, relaxed flag: reading flag == 1 does not
// order the data, so data may still read 0. Source: C11 5.1.2.4 (no
// synchronizes-with without release/acquire); herd7 C11 MP; Lahav et al.,
// PLDI 2017, sec. 2.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int data, flag;
void *p(void *q) { (void)q; atomic_store_explicit(&data, 42, memory_order_relaxed); atomic_store_explicit(&flag, 1, memory_order_relaxed); return 0; }
void *c(void *q) { (void)q; if (atomic_load_explicit(&flag, memory_order_relaxed) == 1) assert(atomic_load_explicit(&data, memory_order_relaxed) == 42); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
