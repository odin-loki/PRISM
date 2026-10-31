// FAILED (CONC-DEADLOCK): the signal can come before the wait, which then
// never returns (no predicate loop).
#include <pthread.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_mutex_lock(&m);
    pthread_cond_wait(&cv, &m);
    pthread_mutex_unlock(&m);
    pthread_join(t, 0);
    return 0;
}
