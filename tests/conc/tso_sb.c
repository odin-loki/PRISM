// Store buffering litmus. --memory-model sc: BOUNDED (r1 == r2 == 0 impossible).
// --memory-model tso: FAILED assertion (both loads read 0 through the store buffers).
// Plain accesses race; races are reported in both modes, the assert result is what differs.
#include <pthread.h>
#include <assert.h>
int x, y, r1, r2;
void *a(void *p) { (void)p; x = 1; r1 = y; return 0; }
void *b(void *p) { (void)p; y = 1; r2 = x; return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r1 == 0 && r2 == 0)); return 0; }
