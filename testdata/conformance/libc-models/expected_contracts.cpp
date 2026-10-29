// std::expected operational model (src/prism/pir/models/cxx/expected) against
// C++23 [expected]. The pir stage puts the model headers first on the include
// path, so <expected> below is the model. Each function is one contract;
// `_false` ones must fail for the class their yml names.
#include <expected>

// value() on a success expected is the stored T.
int expected_value_true(int c) {
    std::expected<int, int> e(c);
    if (!e.has_value()) return 0;
    return e.value();
}

// error() on an unexpected is the stored E.
int expected_error_true(int c) {
    std::expected<int, int> e{std::unexpected<int>(c)};  // braces: not a function declaration
    if (e.has_value()) return 0;
    return e.error();
}

// value() when the expected holds an E (precondition has_value()).
int expected_value_false(int c) {
    std::expected<int, int> e{std::unexpected<int>(c)};  // braces: not a function declaration
    return e.value();
}

// error() when the expected holds a T (precondition !has_value()).
int expected_error_false(int c) {
    std::expected<int, int> e(c);
    return e.error();
}
