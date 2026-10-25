// norace: false. main reads the result before joining the writer.
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
    int r = result;
    pthread_join(t, 0);
    return r;
}
