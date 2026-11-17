// EXPECT: BOUNDED. Kernel MP where the producer and consumer order their accesses with spin_lock / spin_unlock.
// Source: linux tools/memory-model/litmus-tests/MP+polocks.litmus (Result: Never).
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
void spin_lock(int *l);    // LKMM spin_lock: acquire on the lock (linux-kernel.def)
void spin_unlock(int *l);  // LKMM spin_unlock: release on the lock (linux-kernel.def)
int buf, flag, lk, r0, r1;
void *p(void *q) { (void)q; WRITE_ONCE(buf, 1); spin_lock(&lk); WRITE_ONCE(flag, 1); spin_unlock(&lk); return 0; }
void *c(void *q) { (void)q; spin_lock(&lk); r0 = READ_ONCE(flag); spin_unlock(&lk); r1 = READ_ONCE(buf); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r0 == 1 && r1 == 0)); return 0; }
