// EXPECT: FAILED. Kernel MP with only READ_ONCE/WRITE_ONCE: no ordering.
// Source: linux tools/memory-model/litmus-tests/MP+poonceonces.litmus (Result: Sometimes).
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
int buf, flag;
void *p(void *q) { (void)q; WRITE_ONCE(buf, 1); WRITE_ONCE(flag, 1); return 0; }
void *c(void *q) { (void)q; int r0 = READ_ONCE(flag); int r1 = READ_ONCE(buf); assert(!(r0 == 1 && r1 == 0)); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0);  return 0; }
