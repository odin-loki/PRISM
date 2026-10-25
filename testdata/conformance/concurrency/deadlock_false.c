// nodeadlock: false. Two locks taken in opposite orders.
#include <pthread.h>

pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;
int shared = 0;

void *t_ab(void *arg) {
    (void)arg;
    pthread_mutex_lock(&a);
    pthread_mutex_lock(&b);
    shared = shared + 1;
    pthread_mutex_unlock(&b);
    pthread_mutex_unlock(&a);
    return 0;
}

void *t_ba(void *arg) {
    (void)arg;
    pthread_mutex_lock(&b);
    pthread_mutex_lock(&a);
    shared = shared + 2;
    pthread_mutex_unlock(&a);
    pthread_mutex_unlock(&b);
    return 0;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, t_ab, 0);
    pthread_create(&t2, 0, t_ba, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    return 0;
}
