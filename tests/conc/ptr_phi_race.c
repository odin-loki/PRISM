// FAILED (race on a[..]): a pointer chosen by a phi/select into a shared array is written by two threads.
#include <pthread.h>
int a[4];
int flag;
void *w(void *x) { (void)x; int *p = flag ? &a[1] : &a[3]; *p += 1; return 0; }
int main(void) { pthread_t t1, t2; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0); return 0; }
