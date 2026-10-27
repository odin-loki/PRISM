// PRISM conformance task cxx/cxx_format_true.cpp: expected true (no-null-deref)
// format_to_n of a small int into a buffer large enough; the digits are checked
#include <format>
int cxx_format_true(int c) {
    if (c < 0) return 0;
    char buf[8] = {};
    auto r = std::format_to_n(buf, sizeof buf, "{}", 42);
    if (r.size != 2 || buf[0] != '4' || buf[1] != '2') return 0;
    return 1;
}
