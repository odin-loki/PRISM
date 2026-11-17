// EXPECT: NEEDS-HARNESS. Kernel MP with an external call between the producer's accesses.
// ext_sync() is not defined in the module, so it may synchronise or write buf. The PIR translation
// refuses an undefined call in a thread ("UNENCODED: call @ext_sync"), so no BOUNDED can rest on it.
// Source: the MP shape of linux tools/memory-model/litmus-tests/MP+poonceonces.litmus (Result: Sometimes),
// with an opaque call in the middle. docs/KERNEL_MEMORY_MODEL.md section 4 (unrecognised calls).
#include <pthread.h>
#include <assert.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
void ext_sync(void);  // external: no body in this unit
int buf, flag, r0, r1;
void *p(void *q) { (void)q; WRITE_ONCE(buf, 1); ext_sync(); WRITE_ONCE(flag, 1); return 0; }
void *c(void *q) { (void)q; r0 = READ_ONCE(flag); r1 = READ_ONCE(buf); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, p, 0); pthread_create(&b, 0, c, 0);
    pthread_join(a, 0); pthread_join(b, 0); assert(!(r0 == 1 && r1 == 0)); return 0; }
