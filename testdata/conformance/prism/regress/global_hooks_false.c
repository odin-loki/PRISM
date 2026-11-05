/* PRISM conformance task regress/global_hooks_false.c: expected false (no-fp-cast) */
// regression (cJSON_CreateNumber): a struct of allocator hooks initialised
// with malloc/free is mutable (global_hooks_set), so with arbitrary globals
// the call through `allocate` may be a null call; with the initializers it is
// not. The re-check with the initial values stops at another violation, the
// unguarded (int) conversion: that is the defect to report, never the null
// call (which needs a hook other than its initializer).
#include <stdlib.h>
typedef struct hooks {
    void *(*allocate)(size_t size);
    void (*deallocate)(void *pointer);
} hooks;
static hooks global_hooks = {malloc, free};
typedef struct item {
    int valueint;
    double valuedouble;
} item;
void global_hooks_set(void *(*m)(size_t)) { global_hooks.allocate = m; }
static item *new_item(const hooks *const h) { return (item *)h->allocate(sizeof(item)); }
int global_hooks_false(int x) {
    item *it = new_item(&global_hooks);
    if (!it) return 0;
    double num = x * 1000.0;
    it->valuedouble = num;
    if (num >= 1e12)
        it->valueint = 2147483647;
    else
        it->valueint = (int)num; /* |num| > 2^31 for |x| > 2147483 */
    int r = it->valueint;
    global_hooks.deallocate(it);
    return r;
}
