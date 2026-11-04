// BOUNDED (no race): one thread locks the global mutex by name, the other through a pointer to it.
#include <pthread.h>
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int n[1];
void *a(void *x) { (void)x; pthread_mutex_lock(&m); n[0]++; pthread_mutex_unlock(&m); return 0; }
void *b(void *x) { pthread_mutex_t *p = x; pthread_mutex_lock(p); n[0]++; pthread_mutex_unlock(p); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, &m);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
