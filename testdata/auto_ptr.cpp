#include <memory>

void auto_ptr_bad(void) {
    std::auto_ptr<int> p(new int(1));
}

void auto_ptr_ok(void) {
    std::unique_ptr<int> p(new int(1));
}
