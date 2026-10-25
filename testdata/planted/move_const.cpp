#include <utility>

int move_const_bad(void) {
    const int x = 1;
    return std::move(x);
}

int move_const_ok(void) {
    int x = 1;
    return std::move(x);
}
