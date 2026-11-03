// FAILED assertion: the CAS on a heap word succeeds for exactly one thread, so
// "both won" is impossible but "the loser saw 1" is reachable: the assert that
// nobody loses fails.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <assert.h>
void *try_claim(void *p) {
    int z = 0;
    int won = atomic_compare_exchange_strong((atomic_int *)p, &z, 1);
    assert(won);
    return 0;
}
int main(void) {
    atomic_int *w = malloc(sizeof *w);
    if (!w) return 0;
    atomic_store(w, 0);
    pthread_t a, b;
    pthread_create(&a, 0, try_claim, w);
    pthread_create(&b, 0, try_claim, w);
    pthread_join(a, 0);
    pthread_join(b, 0);
    free(w);
    return 0;
}
