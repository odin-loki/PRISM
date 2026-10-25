#include <memory>

void shared_get_bad(void) {
    std::shared_ptr<int> u(new int(1));
    int *p = u.get();
    u.reset();
    *p = 2;
}

void shared_get_ok(void) {
    std::shared_ptr<int> u(new int(1));
    int *p = u.get();
    *p = 2;
    u.reset();
}
