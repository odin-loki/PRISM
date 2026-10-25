#include <memory>

void unique_reset_bad(void) {
    std::unique_ptr<int> u(new int(1));
    int *p = u.get();
    u.reset();
    *p = 2;
}

void unique_reset_ok(void) {
    std::unique_ptr<int> u(new int(1));
    int *p = u.get();
    *p = 2;
    u.reset();
}

void unique_reset_no_raw(void) {
    std::unique_ptr<int> u(new int(1));
    u.reset();
}
