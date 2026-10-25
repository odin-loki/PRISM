#include <optional>

int opt_star_bad(void) {
    optional<int> o;
    return *o;
}

int opt_star_ok(void) {
    optional<int> o;
    if (o.has_value())
        return *o;
    return 0;
}
