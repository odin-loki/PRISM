#include <variant>

int variant_get_bad(void) {
    variant<int, char> v;
    return std::get<int>(v);
}

int variant_get_ok(void) {
    variant<int, char> v;
    if (holds_alternative<int>(v))
        return std::get<int>(v);
    return 0;
}
