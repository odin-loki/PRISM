// FAILED CONC-DATA-RACE on g: bump() is called with &g by both threads and
// with NULL by main (a null call site no longer blocks the pointer parameter).
#include <pthread.h>
int g;
static void bump(int *p) { if (p) *p += 1; }
void *worker(void *a) { (void)a; bump(&g); return 0; }
int main(void) {
    pthread_t t1, t2;
    bump(0);
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    return g;
}
