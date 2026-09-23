/* Correct: the free before `break` leaves the loop; the loop exit path
 * never touches the buffer again, and the other path frees once. */
#include <stdlib.h>

int break_free(int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) {
        int *v = malloc(sizeof *v);
        if (!v)
            return -1;
        *v = i;
        if (*v > 10) {
            free(v);
            break;
        }
        total += *v;
        free(v);
    }
    return total;
}
