// PRISM conformance task cxx/cxx_string_view_false.cpp: expected false (memsafety)
// std::string_view::operator[] one past the end
#include <string_view>
int cxx_string_view_false(int i) {
    const char buf[] = {'a', 'b', 'c'};
    std::string_view v(buf, 3);
    if (i < 0 || i > 3) return 0;
    return v[static_cast<std::size_t>(i)];
}
