// std::variant operational model (src/prism/pir/models/cxx/variant). The pir
// stage puts the model headers first on the include path, so <variant> below
// is the model. Each function is one contract; the _false one gets an
// inactive alternative.
#include <variant>

int variant_active_true(int c) {
    std::variant<int, char> v;
    if (c) {
        v.emplace<int>(c);
        return std::get<int>(v);
    }
    v.emplace<char>('a');
    return std::get<char>(v);
}

int variant_inactive_false(int c) {
    std::variant<int, char> v;
    v.emplace<int>(1);
    if (!c) v.emplace<char>('a');
    return std::get<char>(v);
}
