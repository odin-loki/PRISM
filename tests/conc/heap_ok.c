// BOUNDED (no violation): allocate in a loop (heap_slots = unwind), use and free each in one thread.
#include <pthread.h>
#include <stdlib.h>
int done[2];
void *w(void *x) { (void)x; for (int i = 0; i < 2; ++i) { int *p = malloc(8); p[1] = i; done[i] = p[1]; free(p); } return 0; }
int main(void) { pthread_t t; pthread_create(&t, 0, w, 0); pthread_join(t, 0); return 0; }
