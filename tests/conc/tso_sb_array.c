// Store buffering through a shared array: --memory-model tso --store-buffer 1 -> FAILED assertion; sc -> no assertion failure.
#include <pthread.h>
#include <assert.h>
int m[2], r[2];
void *a(void *p) { (void)p; m[0] = 1; r[0] = m[1]; return 0; }
void *b(void *p) { (void)p; m[1] = 1; r[1] = m[0]; return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); assert(!(r[0] == 0 && r[1] == 0)); return 0; }
