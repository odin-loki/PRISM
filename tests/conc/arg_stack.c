// FAILED (race): main passes the address of its local counter to two threads.
#include <pthread.h>
void *w(void *p) { int *c = p; *c += 1; return 0; }
int main(void) { int cnt = 0; pthread_t t1, t2; pthread_create(&t1, 0, w, &cnt); pthread_create(&t2, 0, w, &cnt);
    pthread_join(t1, 0); pthread_join(t2, 0); return cnt; }
