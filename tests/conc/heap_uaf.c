// FAILED (MEM-USE-AFTER-FREE): main frees the buffer while the worker may still write it.
#include <pthread.h>
#include <stdlib.h>
int *buf[1];
void *w(void *x) { (void)x; buf[0][0] = 1; return 0; }
int main(void) { buf[0] = malloc(8); pthread_t t; pthread_create(&t, 0, w, 0); free(buf[0]);
    pthread_join(t, 0); return 0; }
