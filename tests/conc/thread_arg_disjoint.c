// BOUNDED (no race): each thread gets its own element through its argument.
#include <pthread.h>
int cnt[2];
void *w(void *p) { int *c = p; *c = *c + 1; return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, w, &cnt[0]); pthread_create(&t2, 0, w, &cnt[1]);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
