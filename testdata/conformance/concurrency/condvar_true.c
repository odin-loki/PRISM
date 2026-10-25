// noassert / norace: true. The consumer waits for `ready` in a loop under
// the mutex (spurious wake-ups are handled), so it always sees data == 42.
#include <assert.h>
#include <pthread.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
int ready = 0;
int data = 0;

void *producer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    data = 42;
    ready = 1;
    pthread_cond_signal(&c);
    pthread_mutex_unlock(&m);
    return 0;
}

void *consumer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    while (!ready) pthread_cond_wait(&c, &m);
    assert(data == 42);
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t p, q;
    pthread_create(&q, 0, consumer, 0);
    pthread_create(&p, 0, producer, 0);
    pthread_join(p, 0);
    pthread_join(q, 0);
    return 0;
}
