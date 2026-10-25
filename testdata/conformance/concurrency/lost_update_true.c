// noassert: true. The same read-modify-write under a mutex keeps both updates.
#include <assert.h>
#include <pthread.h>

int counter = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    int tmp = counter;
    counter = tmp + 1;
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    assert(counter == 2);
    return 0;
}
