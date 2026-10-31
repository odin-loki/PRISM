// FAILED assertion: store buffering with release/acquire atomics on the heap;
// both loads may read 0 under C11.
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int *x, *y;
int r1 = 1, r2 = 1;
void *t1(void *p) { (void)p; atomic_store_explicit(x, 1, memory_order_release); r1 = atomic_load_explicit(y, memory_order_acquire); return 0; }
void *t2(void *p) { (void)p; atomic_store_explicit(y, 1, memory_order_release); r2 = atomic_load_explicit(x, memory_order_acquire); return 0; }
int main(void) { x = malloc(sizeof *x); y = malloc(sizeof *y); atomic_init(x, 0); atomic_init(y, 0);
    pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(r1 || r2); return 0; }
