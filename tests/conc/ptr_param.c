// FAILED (race on a[..]): the shared element is written through a helper taking a pointer parameter.
#include <pthread.h>
int a[2];
static void bump(int *p) { *p = *p + 1; }
void *w(void *x) { (void)x; bump(&a[1]); return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
