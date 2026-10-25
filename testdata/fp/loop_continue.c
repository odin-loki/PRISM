/* Correct: the free on the skip path continues to the next iteration;
 * the fall-through path frees once. */
#include <stdlib.h>
#include <string.h>

int loop_continue(const char **items, int n)
{
    int kept = 0;
    for (int i = 0; i < n; i++) {
        char *s = malloc(strlen(items[i]) + 1);
        if (s == NULL)
            continue;
        memcpy(s, items[i], strlen(items[i]) + 1);
        if (s[0] == '#') {
            free(s);
            continue;
        }
        kept += s[0] != 0;
        free(s);
    }
    return kept;
}
