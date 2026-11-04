// FAILED assertion: both locations atomic and relaxed. Seeing y == 1 says
// nothing about x, which may still read its initial 0 (allowed by C11).
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
void *w(void *p) { (void)p; atomic_store_explicit(&x, 1, memory_order_relaxed); atomic_store_explicit(&y, 1, memory_order_relaxed); return 0; }
void *r(void *p) {
    (void)p;
    if (atomic_load_explicit(&y, memory_order_relaxed) == 1) assert(atomic_load_explicit(&x, memory_order_relaxed) == 1);
    return 0;
}
int main(void) { pthread_t a, b; pthread_create(&a, 0, w, 0); pthread_create(&b, 0, r, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
