// FAILED (race on g): g is written by name in one thread and through the thread argument in another.
#include <pthread.h>
int g;
void *a(void *p) { (void)p; g = 1; return 0; }
void *b(void *p) { *(int *)p = 2; return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, a, 0); pthread_create(&t2, 0, b, &g);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
