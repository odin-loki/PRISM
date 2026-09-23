/* Correct: a failed trylock returns without unlocking; the error path
 * unlocks and returns; the fall-through unlocks once. */
#include <pthread.h>

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int state;

int try_update(int v)
{
    if (pthread_mutex_trylock(&state_lock) != 0)
        return -1;
    if (v < 0) {
        pthread_mutex_unlock(&state_lock);
        return -2;
    }
    state = v;
    pthread_mutex_unlock(&state_lock);
    return 0;
}

int drain(int n)
{
    int done = 0;
    while (done < n) {
        pthread_mutex_lock(&state_lock);
        if (state == 0) {
            pthread_mutex_unlock(&state_lock);
            break;
        }
        state--;
        pthread_mutex_unlock(&state_lock);
        done++;
    }
    return done;
}
