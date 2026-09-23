/* Unchecked allocation, on one line and on two: both fire. */
#include <stdlib.h>

void unchecked_one_line(void)
{
    int *p = malloc(4); *p = 3;
    free(p);
}

void unchecked_two_lines(void)
{
    int *p = malloc(4);
    *p = 3;
    free(p);
}
