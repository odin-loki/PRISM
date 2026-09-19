#include <pthread.h>

int g;
void *t1(void *p) { g = 1; return p; }
void *t2(void *p) { g = 2; return p; }
void start(void) { pthread_create(0, 0, t1, 0); pthread_create(0, 0, t2, 0); }
