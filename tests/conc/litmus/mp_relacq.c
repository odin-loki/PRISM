// EXPECT: BOUNDED. Message passing with a release store and an acquire load: flag
// == 1 synchronizes with the store, so data reads 42. Source: C11 7.17.3 and 5.1.2.4
// (synchronizes-with); herd7 C11 MP+rel+acq.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int data, flag;
void *p(void *q) { (void)q; atomic_store_explicit(&data, 42, memory_order_relaxed); atomic_store_explicit(&flag, 1, memory_order_release); return 0; }
void *c(void *q) { (void)q; if (atomic_load_explicit(&flag, memory_order_acquire) == 1) assert(atomic_load_explicit(&data, memory_order_relaxed) == 42); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
