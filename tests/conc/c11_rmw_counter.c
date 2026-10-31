// BOUNDED: relaxed fetch_add is still atomic: two increments give 2 after the joins.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int n;
void *inc(void *p) { (void)p; atomic_fetch_add_explicit(&n, 1, memory_order_relaxed); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, inc, 0); pthread_create(&b, 0, inc, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(atomic_load_explicit(&n, memory_order_relaxed) == 2); return 0; }
