// FAILED MEM-INVALID-FREE: a thread frees a pointer to its creator's local.
#include <pthread.h>
#include <stdlib.h>
void *worker(void *p) { free(p); return 0; }
int main(void) {
    int local = 1;
    pthread_t t;
    pthread_create(&t, 0, worker, &local);
    pthread_join(t, 0);
    return local;
}
