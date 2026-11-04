// FAILED (CONC-DATA-RACE): a thread created by a thread races with main.
#include <pthread.h>

int counter = 0;

void *inner(void *arg) {
    (void)arg;
    counter = counter + 1;
    return 0;
}

void *outer(void *arg) {
    (void)arg;
    pthread_t u;
    pthread_create(&u, 0, inner, 0);
    pthread_join(u, 0);
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, 0, outer, 0);
    counter = counter + 1;
    pthread_join(t, 0);
    return counter;
}
