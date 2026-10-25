#include <span>

span span_dangle_bad(void) {
    int local[4];
    return span<int>(local);
}

int span_ok(span s) {
    return s.size();
}
