// BOUNDED (no deadlock): a heap-allocated mutex + condition variable handshake.
#include <pthread.h>
#include <stdlib.h>
struct Q { pthread_mutex_t m; pthread_cond_t c; int ready; };
struct Q *q[1];
void *w(void *x) { (void)x; pthread_mutex_lock(&q[0]->m); q[0]->ready = 1; pthread_cond_signal(&q[0]->c);
    pthread_mutex_unlock(&q[0]->m); return 0; }
int main(void) { q[0] = calloc(1, sizeof(struct Q)); pthread_t t; pthread_create(&t, 0, w, 0);
    pthread_mutex_lock(&q[0]->m); while (!q[0]->ready) pthread_cond_wait(&q[0]->c, &q[0]->m);
    pthread_mutex_unlock(&q[0]->m); pthread_join(t, 0); free(q[0]); return 0; }
