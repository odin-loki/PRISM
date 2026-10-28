// std::string_view operational model (src/prism/pir/models/cxx/string_view).
// The pir stage puts the model headers first on the include path, so
// <string_view> below is the model. Each function is one contract.
// compare and find are not part of the model.
#include <string_view>

// operator[] in range is the character that was stored.
int sv_index_true(int i) {
    const char buf[] = {'a', 'b', 'c'};
    std::string_view v(buf, 3);
    if (i < 0 || i >= 3) return 0;
    return v[static_cast<std::size_t>(i)];
}

// operator[] one past the end.
int sv_index_false(int i) {
    const char buf[] = {'a', 'b', 'c'};
    std::string_view v(buf, 3);
    if (i < 0 || i > 3) return 0;
    return v[static_cast<std::size_t>(i)];
}

// front() on an empty view.
int sv_front_empty_false(int c) {
    const char b[] = {'x'};
    std::string_view v = c ? std::string_view(b, 1) : std::string_view();
    return v.front();
}

// A null pointer is a failed check, not an empty view.
int sv_null_false(int) {
    const char* p = nullptr;
    std::string_view v(p);
    return static_cast<int>(v.size());
}
