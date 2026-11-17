// EXPECT: BOUNDED. Kernel SB where each thread writes, then brackets an atomic_inc with smp_mb__before_atomic / smp_mb__after_atomic, then reads.
// Source: linux tools/memory-model/linux-kernel.def (smp_mb__before_atomic, smp_mb__after_atomic are fences);
// tools/memory-model/Documentation/explanation.txt, "oddball fences": smp_mb__before_atomic orders
// po-earlier events against po-later atomic updates and the events following them, smp_mb__after_atomic
// orders po-earlier atomic updates and the events preceding them against all po-later events. With both
// on each side, the store is ordered before the load on each thread: SB+ forbidden (Never).
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define atomic_inc(p) __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)  // Documentation/atomic_t.txt: no ordering
void smp_mb__before_atomic(void);  // recognised by name in the conc extractor
void smp_mb__after_atomic(void);
int x, y, a, r0, r1;
void *p(void *q) { (void)q; WRITE_ONCE(x, 1); smp_mb__before_atomic(); atomic_inc(&a); smp_mb__after_atomic(); r0 = READ_ONCE(y); return 0; }
void *c(void *q) { (void)q; WRITE_ONCE(y, 1); smp_mb__before_atomic(); atomic_inc(&a); smp_mb__after_atomic(); r1 = READ_ONCE(x); return 0; }
int main(void) { pthread_t t0, t1; pthread_create(&t0, 0, p, 0); pthread_create(&t1, 0, c, 0);
    pthread_join(t0, 0); pthread_join(t1, 0); assert(!(r0 == 0 && r1 == 0)); return 0; }
