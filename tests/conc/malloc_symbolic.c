// FAILED (MEM-OOB-WRITE): malloc(n) with symbolic n; the worker writes p[4] which is out of bounds when n < 20.
#include <pthread.h>
#include <stdlib.h>
extern unsigned __VERIFIER_nondet_uint(void);
int *p[1];
void *w(void *x) { (void)x; p[0][4] = 1; return 0; }
int main(void) { unsigned n = __VERIFIER_nondet_uint(); if (n < 4) return 0; p[0] = malloc(n);
    pthread_t t; pthread_create(&t, 0, w, 0); pthread_join(t, 0); free(p[0]); return 0; }
