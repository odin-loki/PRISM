// BOUNDED: trylock on a heap mutex; the counter is only touched when the lock was taken.
#include <pthread.h>
#include <stdlib.h>
struct L { pthread_mutex_t m; int n; } *l[1];
void *w(void *x) { (void)x; if (pthread_mutex_trylock(&l[0]->m) == 0) { l[0]->n++; pthread_mutex_unlock(&l[0]->m); } return 0; }
int main(void) { l[0] = calloc(1, sizeof(struct L)); pthread_t t1, t2; pthread_create(&t1, 0, w, 0);
    pthread_create(&t2, 0, w, 0); pthread_join(t1, 0); pthread_join(t2, 0); free(l[0]); return 0; }
