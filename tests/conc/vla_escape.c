// FAILED MEM-OOB-WRITE: a variable-length array shared with a thread, which
// writes one element past its end.
#include <pthread.h>
struct job { int *a; int n; };
void *worker(void *p) { struct job *j = p; j->a[j->n] = 1; return 0; }
int run(int n) {
    int buf[n];
    struct job j = {buf, n};
    pthread_t t;
    pthread_create(&t, 0, worker, &j);
    pthread_join(t, 0);
    return 0;
}
int main(void) { return run(3); }
