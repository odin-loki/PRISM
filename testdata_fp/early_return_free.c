/* Correct: the free inside the early-return block never reaches the code
 * after it. No MEM-UAF, no MEM-DOUBLE-FREE, no MEM-LEAK. */
#include <stdlib.h>

int early_return_free(int c)
{
    char *p = malloc(4);
    if (!p)
        return 0;
    if (c) {
        free(p);
        return 1;
    }
    p[0] = 0;
    free(p);
    return 0;
}

int early_return_one_line(int c){ char *p=malloc(4); if(!p) return 0; if (c) { free(p); return 1; } p[0]=0; free(p); return 0; }

int early_exit_free(int c)
{
    char *p = malloc(8);
    if (p == NULL) {
        return -1;
    }
    if (c < 0) {
        free(p);
        exit(1);
    }
    p[0] = 'x';
    free(p);
    return 0;
}
