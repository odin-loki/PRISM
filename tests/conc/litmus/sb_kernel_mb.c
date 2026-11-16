// EXPECT: BOUNDED. Kernel SB with smp_mb.
// Source: linux tools/memory-model/litmus-tests/SB+fencembonceonces.litmus (Result: Never).
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
int x, y, r0, r1;
void *p(void *q) { (void)q; WRITE_ONCE(x, 1); smp_mb(); r0 = READ_ONCE(y); return 0; }
void *c(void *q) { (void)q; WRITE_ONCE(y, 1); smp_mb(); r1 = READ_ONCE(x); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r0 == 0 && r1 == 0)); return 0; }
