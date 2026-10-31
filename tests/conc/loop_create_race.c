// FAILED (race on total): threads created in a loop (handles in a local array) all add to total unlocked.
#include <pthread.h>
int total[1];
void *w(void *x) { (void)x; total[0] += 1; return 0; }
int main(void) {
    pthread_t t[3];
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < 3; ++i) pthread_join(t[i], 0);
    return 0;
}
