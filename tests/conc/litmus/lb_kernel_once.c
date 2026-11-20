// EXPECT: NEEDS-HARNESS lkmm-no-lb. Kernel LB with no ordering: P0 READ_ONCE(x) then WRITE_ONCE(y);
// P1 READ_ONCE(y) then WRITE_ONCE(x). The LKMM allows r0 == r1 == 1 (Sometimes); the conc model forbids
// it (RC11-style, no LB), so a BOUNDED verdict here would be a claim the kernel does not support.
// Source: linux tools/memory-model/litmus-tests/LB+poonceonces.litmus (Result: Sometimes).
// docs/KERNEL_MEMORY_MODEL.md section 4 (load buffering): the verdict is withheld as UNENCODED.
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
int x, y, r0, r1;
void *p(void *q) { (void)q; r0 = READ_ONCE(x); WRITE_ONCE(y, 1); return 0; }
void *c(void *q) { (void)q; r1 = READ_ONCE(y); WRITE_ONCE(x, 1); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r0 == 1 && r1 == 1)); return 0; }
