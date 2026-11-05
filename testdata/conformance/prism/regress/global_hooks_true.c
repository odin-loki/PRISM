/* PRISM conformance task regress/global_hooks_true.c: expected true (no-fp-cast) */
// regression (cJSON_CreateNumber): the allocator hooks are mutable
// (global_hooks_set), so the call through `allocate` is a null call only for
// a hook other than its initializer, and the conversion saturates: no
// violation with the initial values. The verdict is NEEDS-HARNESS (the
// global state is an unstated precondition), never a FAILED null call.
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
int global_hooks_true(int x) {
    item *it = new_item(&global_hooks);
    if (!it) return 0;
    double num = x * 1000.0;
    it->valuedouble = num;
    if (num >= 2147483647.0)
        it->valueint = 2147483647;
    else if (num <= -2147483648.0)
        it->valueint = -2147483647 - 1;
    else
        it->valueint = (int)num;
    int r = it->valueint;
    global_hooks.deallocate(it);
    return r;
}
