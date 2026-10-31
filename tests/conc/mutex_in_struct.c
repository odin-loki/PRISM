// BOUNDED (no race): the counter is protected by a mutex that is a field of a global struct.
#include <pthread.h>
struct Acc { pthread_mutex_t m; int n; } acc = { PTHREAD_MUTEX_INITIALIZER, 0 };
void *w(void *x) { (void)x; pthread_mutex_lock(&acc.m); acc.n++; pthread_mutex_unlock(&acc.m); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
