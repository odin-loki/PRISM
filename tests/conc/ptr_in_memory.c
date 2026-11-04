// FAILED (race): a pointer stored in a shared struct and loaded back by both threads.
#include <pthread.h>
struct S { int *p; int v[2]; } s;
void *w(void *x) { (void)x; *s.p = *s.p + 1; return 0; }
int main(void) { pthread_t t1, t2; s.p = &s.v[1]; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
