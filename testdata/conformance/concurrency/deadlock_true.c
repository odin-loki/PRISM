// nodeadlock: true. Both threads take the two locks in the same order.
#include <pthread.h>

pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;
int shared = 0;

void *t1_body(void *arg) {
    (void)arg;
    pthread_mutex_lock(&a);
    pthread_mutex_lock(&b);
    shared = shared + 1;
    pthread_mutex_unlock(&b);
    pthread_mutex_unlock(&a);
    return 0;
}

void *t2_body(void *arg) {
    (void)arg;
    pthread_mutex_lock(&a);
    pthread_mutex_lock(&b);
    shared = shared + 2;
    pthread_mutex_unlock(&b);
    pthread_mutex_unlock(&a);
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, t1_body, 0);
    pthread_create(&t2, 0, t2_body, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    return 0;
}
