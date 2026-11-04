// FAILED CONC-DATA-RACE on x: a plain store to an atomic location races with
// a relaxed load of it in another thread.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x;
void *t1(void *p) { (void)p; *(int *)&x = 1; return 0; }
void *t2(void *p) { (void)p; return (void *)(long)atomic_load_explicit(&x, memory_order_relaxed); }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
