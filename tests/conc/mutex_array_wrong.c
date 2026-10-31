// FAILED (race on n): threads lock different elements of a mutex array around the same counter.
#include <pthread.h>
pthread_mutex_t ms[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
int n[1];
void *w(void *x) { pthread_mutex_t *m = x; pthread_mutex_lock(m); n[0]++; pthread_mutex_unlock(m); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, w, &ms[0]); pthread_create(&t2, 0, w, &ms[1]);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
