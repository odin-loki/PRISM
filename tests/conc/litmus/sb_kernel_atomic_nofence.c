// EXPECT: FAILED. Kernel SB with an unordered atomic_inc between the store and the load, and no barrier.
// Source: linux tools/memory-model/litmus-tests/SB+poonceonces.litmus (Result: Sometimes), with the
// accesses to x and y as WRITE_ONCE/READ_ONCE. The RMW on a is non-value-returning and carries no
// ordering (Documentation/atomic_t.txt), so it does not order the store before the load.
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define atomic_inc(p) __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)
int x, y, a, r0, r1;
void *p(void *q) { (void)q; WRITE_ONCE(x, 1); atomic_inc(&a); r0 = READ_ONCE(y); return 0; }
void *c(void *q) { (void)q; WRITE_ONCE(y, 1); atomic_inc(&a); r1 = READ_ONCE(x); return 0; }
int main(void) { pthread_t t0, t1; pthread_create(&t0, 0, p, 0); pthread_create(&t1, 0, c, 0);
    pthread_join(t0, 0); pthread_join(t1, 0); assert(!(r0 == 0 && r1 == 0)); return 0; }
