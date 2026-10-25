/* Correct: each path releases the mutex exactly once. */
#include <pthread.h>

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int counter;

int lock_branches(int c)
{
    pthread_mutex_lock(&lk);
    if (c) {
        pthread_mutex_unlock(&lk);
        return 1;
    }
    counter++;
    pthread_mutex_unlock(&lk);
    return 0;
}

int lock_goto(int c)
{
    int rc = 0;
    pthread_mutex_lock(&lk);
    if (c < 0) {
        rc = -1;
        pthread_mutex_unlock(&lk);
        goto out;
    }
    counter += c;
    pthread_mutex_unlock(&lk);
out:
    return rc;
}

void lock_loop(int n)
{
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&lk);
        if (counter > 100) {
            pthread_mutex_unlock(&lk);
            continue;
        }
        counter++;
        pthread_mutex_unlock(&lk);
    }
}
