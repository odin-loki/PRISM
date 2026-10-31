// FAILED assertion under --memory-model tso (and under SC): the 16-bit load of
// the first half of buf[0] is forwarded byte-wise from the buffered 32-bit
// store, so it reads 0x5678 (little-endian), not 0.
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
uint32_t buf[2];
void *t1(void *p) { (void)p; buf[0] = 0x12345678u; uint16_t lo = ((volatile uint16_t *)buf)[0]; assert(lo == 0); return 0; }
int main(void) { pthread_t a; pthread_create(&a, 0, t1, 0); pthread_join(a, 0); return 0; }
