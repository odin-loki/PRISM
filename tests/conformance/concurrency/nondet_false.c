// noassert: false. Same program, but the bound is 10 inclusive: 10 + 10
// reaches 20 for some nondeterministic inputs.
#include <assert.h>
#include <pthread.h>

extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);

int total = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg) {
    (void)arg;
    int k = __VERIFIER_nondet_int();
    __VERIFIER_assume(k >= 0 && k <= 10);
    pthread_mutex_lock(&m);
    total = total + k;
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    assert(total < 20);
    return 0;
}
