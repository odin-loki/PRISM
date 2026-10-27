// PRISM conformance task cxx/cxx_format_false.cpp: expected false (no-null-deref)
// format_to_n with a null output pointer and n > 0
#include <format>
int cxx_format_false(int n) {
    if (n <= 0) return 0;
    auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(n), "{}", 42);
    return static_cast<int>(r.size);
}
