// FAILED assertion: a 32-bit global written through a 16-bit view (mixed
// widths go to the byte-addressed memory); the low half changes, so the
// value is no longer 0x10000.
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
uint32_t g = 0x10000;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void *t1(void *p) { (void)p; pthread_mutex_lock(&m); *(volatile uint16_t *)&g = 5; pthread_mutex_unlock(&m); return 0; }
int main(void) { pthread_t a; pthread_create(&a, 0, t1, 0); pthread_join(a, 0); assert(g == 0x10000); return 0; }
