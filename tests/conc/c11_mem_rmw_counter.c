// BOUNDED: relaxed fetch_add on an address-taken atomic counter; RMWs read the
// latest value, so after the joins the counter is 2.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int cnt;
atomic_int *pc = &cnt;
void *t1(void *p) { (void)p; atomic_fetch_add_explicit(pc, 1, memory_order_relaxed); return 0; }
void *t2(void *p) { (void)p; atomic_fetch_add_explicit(&cnt, 1, memory_order_relaxed); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(atomic_load_explicit(&cnt, memory_order_relaxed) == 2); return 0; }
