#include <memory>

void unique_release_bad(void) {
    std::unique_ptr<int> u(new int(1));
    u.release();
    int *p = u.get();
    *p = 2;
}

void unique_release_ok(void) {
    std::unique_ptr<int> u(new int(1));
    int *p = u.release();
    *p = 2;
}
