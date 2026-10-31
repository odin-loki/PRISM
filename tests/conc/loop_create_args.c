// BOUNDED (no race), extra loop_thread_slots reported: each looped thread writes its own slot via its argument.
#include <pthread.h>
int out[3];
void *w(void *x) { int *p = x; *p = 1; return 0; }
int main(void) {
    pthread_t t[3];
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], 0, w, &out[i]);
    for (int i = 0; i < 3; ++i) pthread_join(t[i], 0);
    return 0;
}
