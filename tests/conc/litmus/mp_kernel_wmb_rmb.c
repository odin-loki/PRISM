// EXPECT: NEEDS-HARNESS lkmm-rmb-strong. Kernel MP with smp_wmb / smp_rmb around READ_ONCE/WRITE_ONCE.
// Source: linux tools/memory-model/litmus-tests/MP+fencewmbonceonce+fencermbonceonce.litmus (Result: Never).
// The LKMM forbids the outcome, and the model's acquire/release fences do too; but the model's smp_rmb is
// an acquire fence (stronger than the LKMM's read-read fence) and smp_wmb a release fence, so the BOUNDED
// claim is withheld, naming both (docs/KERNEL_MEMORY_MODEL.md section 4).
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
void *p(void *q) { (void)q; WRITE_ONCE(buf, 1); smp_wmb(); WRITE_ONCE(flag, 1); return 0; }
void *c(void *q) { (void)q; int r0 = READ_ONCE(flag); smp_rmb(); int r1 = READ_ONCE(buf); assert(!(r0 == 1 && r1 == 0)); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0);  return 0; }
