// noassert: false. The "lock" is a plain test-then-set, not atomic: both
// threads can see it free and enter the critical section together.
#include <assert.h>
#include <pthread.h>

int busy = 0;
int inside = 0;

void *worker(void *arg) {
    (void)arg;
    if (busy == 0) {
        busy = 1;
        inside = inside + 1;
        assert(inside == 1);
        inside = inside - 1;
        busy = 0;
    }
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
