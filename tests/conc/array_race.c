// FALSE (norace): two threads write the same element of a shared global array
// (bounded memory, docs/CONCURRENCY.md "Memory").
#include <pthread.h>

int buf[4];

void *worker(void *arg) {
    (void)arg;
    buf[2] = buf[2] + 1;
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
