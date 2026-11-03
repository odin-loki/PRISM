// FAILED assertion: store buffering with release/acquire. RA does not order a
// store before a later load of another location, so r1 == r2 == 0 is allowed.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1;
void *a(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_release); r1 = atomic_load_explicit(&y, memory_order_acquire); return 0; }
void *b(void *p) { (void)p; atomic_store_explicit(&y, 1, memory_order_release); r2 = atomic_load_explicit(&x, memory_order_acquire); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r1 == 0 && r2 == 0)); return 0; }
