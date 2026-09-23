// TRUE (norace, noassert): main reads the result only after joining.
#include <assert.h>
#include <pthread.h>

int result = 0;

void *worker(void *arg) {
    (void)arg;
    result = 42;
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_join(t, 0);
    assert(result == 42);
    return 0;
}
