// FAILED (MEM-DOUBLE-FREE): both threads free the same allocation.
#include <pthread.h>
#include <stdlib.h>
int *buf[1];
void *w(void *x) { (void)x; free(buf[0]); return 0; }
int main(void) { buf[0] = malloc(4); pthread_t t1, t2; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
