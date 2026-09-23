/* Correct: the free in the error case returns; the other cases fall out
 * of the switch with the buffer still live. */
#include <stdlib.h>

int switch_free(int mode)
{
    char *p = malloc(32);
    if (!p)
        return -1;
    switch (mode) {
    case 0: {
        free(p);
        return -2;
    }
    case 1:
        p[0] = 1;
        break;
    default:
        p[0] = 2;
        break;
    }
    p[1] = 0;
    free(p);
    return 0;
}
