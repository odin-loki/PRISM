// FALSE (norace): two threads increment a shared counter without a lock.
#include <pthread.h>

int counter = 0;

void *worker(void *arg) {
    (void)arg;
    counter = counter + 1;
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    return 0;
}
