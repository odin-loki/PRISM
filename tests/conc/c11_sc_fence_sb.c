// BOUNDED: store buffering with relaxed accesses separated by seq_cst fences;
// RC11 forbids r1 == r2 == 0.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = 1, r2 = 1;
void *t1(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); atomic_thread_fence(memory_order_seq_cst);
    r1 = atomic_load_explicit(&y, memory_order_relaxed); return 0; }
void *t2(void *p) { (void)p; atomic_store_explicit(&y, 1, memory_order_relaxed); atomic_thread_fence(memory_order_seq_cst);
    r2 = atomic_load_explicit(&x, memory_order_relaxed); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(r1 || r2); return 0; }
