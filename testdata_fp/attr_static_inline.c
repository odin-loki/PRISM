/* Correct: attributes and static inline heads parse; bodies are clean. */
#include <stddef.h>

__attribute__((noinline)) static int attr_first(int a)
{
    return a > 0 ? a : 0;
}

static inline int sinl(int a)
{
    return attr_first(a) + 1;
}

static int (*pick(int which))(int)
{
    return which ? sinl : attr_first;
}

int run_pick(int w, int v)
{
    int (*fn)(int) = pick(w);
    return fn(v);
}
