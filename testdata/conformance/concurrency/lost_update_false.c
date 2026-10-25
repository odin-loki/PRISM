// noassert: false. The unprotected read-modify-write loses an update.
#include <assert.h>
#include <pthread.h>

int counter = 0;

void *worker(void *arg) {
    (void)arg;
    int tmp = counter;
    counter = tmp + 1;
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
