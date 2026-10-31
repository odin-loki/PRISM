// FAILED (race): a helper (not the harness) creates two threads sharing its local, then returns.
#include <pthread.h>
void *w(void *p) { int *c = p; *c += 1; return 0; }
static void run(void) { int local = 0; pthread_t t1, t2; pthread_create(&t1, 0, w, &local);
    pthread_create(&t2, 0, w, &local); pthread_join(t1, 0); pthread_join(t2, 0); }
int main(void) { run(); return 0; }
