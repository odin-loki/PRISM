// BOUNDED: the wait is guarded by its predicate, so a signal sent first is
// not lost.
#include <pthread.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
int ready = 0;

void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_mutex_lock(&m);
    while (!ready) pthread_cond_wait(&cv, &m);
    pthread_mutex_unlock(&m);
    pthread_join(t, 0);
    return 0;
}
