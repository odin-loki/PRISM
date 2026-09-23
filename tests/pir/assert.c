/* PIR tasks: assert() reachability. */
#include <assert.h>
int assert_bad(int x) {
    assert(x != 3);
    return x;
}
int assert_ok(int x) {
    if (x == 3)
        return 0;
    assert(x != 3);
    return x;
}
