// EXPECT: BOUNDED. Store buffering, all seq_cst: r1 == r2 == 0 is forbidden.
// Source: C11 7.17.3 (a single total order S of seq_cst operations); herd7 C11
// catalogue SB+scs; Batty et al., POPL 2011, sec. 4.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int x, y;
int r1 = -1, r2 = -1;
void *a(void *p) { (void)p; atomic_store(&x, 1); r1 = atomic_load(&y); return 0; }
void *b(void *p) { (void)p; atomic_store(&y, 1); r2 = atomic_load(&x); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r1 == 0 && r2 == 0)); return 0; }
