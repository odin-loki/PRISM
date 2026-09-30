#include <stddef.h>
#include <string.h>

struct item {
    char *name;
};

size_t null_member_bad(struct item *it) {
    return strlen(it->name);
}

size_t null_member_ok(struct item *it) {
    if (it->name == NULL)
        return 0;
    return strlen(it->name);
}
