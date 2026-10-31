// BOUNDED: atomic RMW and compare-exchange on a global whose address is taken
// (it lives in the bounded memory): two increments through a pointer and one
// CAS by name keep the count exact.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int n;
void *inc(void *p) { atomic_fetch_add((atomic_int *)p, 1); return 0; }
int main(void) {
    pthread_t a, b;
    pthread_create(&a, 0, inc, &n);
    pthread_create(&b, 0, inc, &n);
    pthread_join(a, 0);
    pthread_join(b, 0);
    int two = 2;
    assert(atomic_compare_exchange_strong(&n, &two, 5));
    assert(atomic_load(&n) == 5);
    return 0;
}
