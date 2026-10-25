// noassert: false. The consumer waits with `if`, not `while`: a spurious
// wake-up (allowed by POSIX) lets it read data before the producer ran.
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
    if (!ready) pthread_cond_wait(&c, &m);
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
