#include <utility>

struct Widget {
    void use() {}
};

void move_bad(Widget a) {
    std::move(a);
    a.use();
}

void move_ok(Widget a) {
    std::move(a);
}
