/* The unlock in the branch does not leave, so the second unlock is a
 * double unlock on that path. */
#include <pthread.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int hits;

void unlock_twice(int c)
{
    pthread_mutex_lock(&mu);
    if (c) {
        pthread_mutex_unlock(&mu);
        hits++;
    }
    pthread_mutex_unlock(&mu);
}
