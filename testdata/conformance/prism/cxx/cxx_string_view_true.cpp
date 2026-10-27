// PRISM conformance task cxx/cxx_string_view_true.cpp: expected true (memsafety)
// std::string_view::operator[] in range reads the real character
#include <string_view>
int cxx_string_view_true(int i) {
    const char buf[] = {'a', 'b', 'c'};
    std::string_view v(buf, 3);
    if (i < 0 || i >= 3) return 0;
    return v[static_cast<std::size_t>(i)];
}
